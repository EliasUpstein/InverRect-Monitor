#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

/**
 * ============================================================================
 * Proyecto: InverRect-Monitor - Control de Rectificador con ESP32
 * Archivo: esp32_firmware/esp32_firmware.ino (Sketch compatible con Arduino IDE)
 * Carpeta: esp32_firmware/
 * Arquitectura: Dual-Core (FreeRTOS) + Detección ZCD & ADC + SCADA UDP
 * 
 * Este sketch está estructurado para abrirse e instalarse directamente desde
 * Arduino IDE o PlatformIO. Cumple con la convención de Arduino IDE donde
 * el nombre del archivo principal (.ino) coincide exactamente con el nombre de
 * la carpeta contenedora (esp32_firmware).
 * ============================================================================
 */

// ==========================================
// 1. DEFINICIONES DE HARDWARE Y PINES
// ==========================================
#define PIN_ZERO_CROSS     4   // Entrada digital ZCD: Cruce por cero (Optoacoplador 4N25/PC817 en GPIO 4)
#define PIN_DISPARO        16  // Salida digital Disparo: Compuerta (MOC3021 / TRIAC / SCR en GPIO 16)
#define PIN_POTENTIOMETER  36  // Entrada analógica Potenciómetro: (ADC1_CH0 / VP en GPIO 36)

// Parámetros temporales y de sincronismo de potencia
#define FRECUENCIA_RED_HZ         50.0f
#define PERIODO_CICLO_DEFAULT_US  20000 // 20 ms (360°) para ZCD monopulso por ciclo en red 50 Hz
#define ANCHO_PULSO_DISPARO_US    50    // Ancho del pulso de compuerta en us (50 us)
#define ADELANTO_COMPENSACION_US  100   // Compensación de retardo físico del optoacoplador (100 us)
#define FILTRO_DEBOUNCE_ZCD_US    12000 // Antirrebote ZCD (>12 ms descarta ruidos en ciclo de 20 ms)

// ==========================================
// 1.1 CONFIGURACIÓN DE RED Y COMUNICACIONES (CORE 0)
// ==========================================
const char* WIFI_SSID     = "TU_SSID_AQUI";        // Configurar con el SSID de la red local
const char* WIFI_PASSWORD = "TU_PASSWORD_AQUI";    // Configurar con la clave de la red local
const uint16_t UDP_PORT   = 8888;                  // Puerto UDP local de escucha

WiFiUDP udp;

// Spinlock / MUX para acceso atómico entre Core 0 (Red UDP) y Core 1 (Potencia/ADC/ISR)
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// Manejador de la Tarea FreeRTOS de red
TaskHandle_t ManejadorOTA = NULL;

// ==========================================
// 2. VARIABLES GLOBALES VOLÁTILES
// ==========================================
volatile float angulo_disparo_deg = 90.0f;           // Ángulo de disparo alfa en grados (5.0° a 175.0°)
volatile uint32_t retardo_disparo_us = 5000;         // Retardo equivalente en microsegundos
volatile unsigned long tiempoUltimoCruce = 0;        // Timestamp (micros) del último cruce ZCD válido
volatile unsigned long periodoCicloCompleto = PERIODO_CICLO_DEFAULT_US; // Periodo real medido de la red
volatile bool cruceDetectado = false;               // Bandera de cruce por cero detectado
volatile bool modo_adc_potenciometro = false;        // true = Control por Potenciómetro ADC, false = Control UDP/Manual

// ==========================================
// 3. PROTOTIPOS DE FUNCIONES (Para compatibilidad con C++ / Arduino IDE)
// ==========================================
inline uint32_t calcular_retardo_us(float grados, unsigned long periodo_us);
void set_angulo_disparo_deg(float grados, bool es_modo_adc = false);
void IRAM_ATTR isrCrucePorCero();
void setup_red();
void TaskComunicaciones(void *pvParameters);

// ==========================================
// 4. FUNCIONES DE CÁLCULO Y CONTROL
// ==========================================

/**
 * Convierte un ángulo en grados (5.0° - 175.0°) a retardo en microsegundos (us)
 * utilizando el periodo de ciclo completo medido dinámicamente.
 * Fórmula: retardo_us = ((periodoCicloCompleto * angulo) / 360.0) - ADELANTO_COMPENSACION_US
 */
inline uint32_t calcular_retardo_us(float grados, unsigned long periodo_us) {
    if (grados < 5.0f) grados = 5.0f;
    if (grados > 175.0f) grados = 175.0f;

    float us_base = (periodo_us * grados) / 360.0f;
    if (us_base > ADELANTO_COMPENSACION_US) {
        return (uint32_t)(us_base - ADELANTO_COMPENSACION_US);
    }
    return 10; // Retardo mínimo seguro en us
}

/**
 * Actualiza el ángulo de disparo y recalcula el retardo en microsegundos de forma atómica.
 */
void set_angulo_disparo_deg(float grados, bool es_modo_adc) {
    if (grados < 5.0f) grados = 5.0f;
    if (grados > 175.0f) grados = 175.0f;

    portENTER_CRITICAL(&timerMux);
    angulo_disparo_deg = grados;
    retardo_disparo_us = calcular_retardo_us(grados, periodoCicloCompleto);
    modo_adc_potenciometro = es_modo_adc;
    portEXIT_CRITICAL(&timerMux);
}

// ==========================================
// 5. RUTINA DE INTERRUPCIÓN ZCD (CORE 1 - IRAM)
// ==========================================

/**
 * ISR: Cruce por Cero (Zero Cross Detection)
 * Medición de periodo real de ciclo y filtro antirrebote industrial.
 */
void IRAM_ATTR isrCrucePorCero() {
    unsigned long tiempoActual = micros();
    unsigned long tiempoMedido = tiempoActual - tiempoUltimoCruce;

    // Filtro temporal antirrebote: descarta pulsos espurios antes de 12 ms en ciclo de 20 ms
    if (tiempoMedido > FILTRO_DEBOUNCE_ZCD_US) {
        periodoCicloCompleto = tiempoMedido;
        tiempoUltimoCruce = tiempoActual;
        cruceDetectado = true;
    }
}

// ==========================================
// 6. CONFIGURACIÓN DE RED Y TAREAS (CORE 0)
// ==========================================

void setup_red() {
    Serial.println("[Core 0] Iniciando conexión Wi-Fi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int intentos = 0;
    const int max_intentos = 30; // 15 segundos timeout
    while (WiFi.status() != WL_CONNECTED && intentos < max_intentos) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        intentos++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[Core 0] ¡Wi-Fi conectado con éxito!");
        Serial.print("[Core 0] Dirección IP asignada: ");
        Serial.println(WiFi.localIP());

        // Configuración de seguridad ArduinoOTA
        ArduinoOTA.setHostname("esp32-rmmoc");
        ArduinoOTA.setPassword("edptest");

        ArduinoOTA.onStart([]() {
            // 1. Desvincular interrupción ZCD para prevenir disparos erráticos durante el flasheo
            detachInterrupt(digitalPinToInterrupt(PIN_ZERO_CROSS));
            // 2. Apagar etapa de potencia por seguridad
            digitalWrite(PIN_DISPARO, LOW);
            Serial.println("[OTA] ¡Inicio de flasheo inalámbrico! Interrupciones de potencia suspendidas.");
        });

        ArduinoOTA.onEnd([]() {
            Serial.println("\n[OTA] Actualización completada con éxito. Reiniciando...");
        });

        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("[OTA] Error[%u] al actualizar.\n", error);
        });

        ArduinoOTA.begin();
        udp.begin(UDP_PORT);
        Serial.printf("[Core 0] Servidor UDP a la escucha en el puerto %u.\n", UDP_PORT);
    } else {
        Serial.println("\n[Core 0] Aviso: Timeout Wi-Fi (15s). Modo local/Serial activo.");
    }
}

void TaskComunicaciones(void *pvParameters) {
    (void) pvParameters;
    setup_red();

    char packetBuffer[64];
    uint32_t ultimo_reintento_wifi = 0;
    bool servicios_inicializados = (WiFi.status() == WL_CONNECTED);

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            servicios_inicializados = false;
            uint32_t ahora = millis();
            if (ahora - ultimo_reintento_wifi > 10000) {
                ultimo_reintento_wifi = ahora;
                Serial.println("[Core 0] Reintentando conexión Wi-Fi...");
                WiFi.reconnect();
            }
        } else {
            if (!servicios_inicializados) {
                Serial.println("\n[Core 0] ¡Wi-Fi reconectado!");
                ArduinoOTA.begin();
                udp.begin(UDP_PORT);
                servicios_inicializados = true;
            }

            ArduinoOTA.handle();

            int packetSize = udp.parsePacket();
            if (packetSize > 0) {
                int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
                if (len > 0) {
                    packetBuffer[len] = '\0';
                    String mensaje = String(packetBuffer);
                    mensaje.trim();

                    // Comando para activar modo ADC Potenciómetro: "ALFA:ADC" o "MODO:ADC"
                    if (mensaje.equals("ALFA:ADC") || mensaje.equals("MODO:ADC")) {
                        portENTER_CRITICAL(&timerMux);
                        modo_adc_potenciometro = true;
                        portEXIT_CRITICAL(&timerMux);

                        Serial.println("[Core 0][UDP] Modo ADC Potenciómetro ACTIVADO vía GUI SCADA.");
                        udp.beginPacket(udp.remoteIP(), udp.remotePort());
                        udp.printf("ACK:ALFA:ADC:%.1f\n", angulo_disparo_deg);
                        udp.endPacket();
                    }
                    // Comando con ángulo numérico: "ALFA:xx.x" (ej. "ALFA:60.0")
                    else if (mensaje.startsWith("ALFA:")) {
                        float alfa = mensaje.substring(5).toFloat();
                        if (alfa >= 0.0f && alfa <= 180.0f) {
                            set_angulo_disparo_deg(alfa, false); // Desactiva modo ADC y fija ángulo UDP
                            Serial.printf("[Core 0][UDP] Comando manual aplicado: ALFA = %.1f°\n", alfa);

                            udp.beginPacket(udp.remoteIP(), udp.remotePort());
                            udp.printf("ACK:ALFA:%.1f\n", alfa);
                            udp.endPacket();
                        } else {
                            Serial.printf("[Core 0][UDP] Ángulo fuera de rango (0-180°): %.1f°\n", alfa);
                            udp.beginPacket(udp.remoteIP(), udp.remotePort());
                            udp.printf("NACK:OUT_OF_RANGE:%.1f\n", alfa);
                            udp.endPacket();
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // Retardo de 20 ms para la tarea de red
    }
}

// ==========================================
// 7. SETUP E INICIALIZACIÓN PRINCIPAL (Arduino Standard Entry Point)
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- InverRect-Monitor: Inicializando Firmware ESP32 (Arduino IDE) ---");

    // 1. Configuración de Pines GPIO
    pinMode(PIN_DISPARO, OUTPUT);
    digitalWrite(PIN_DISPARO, LOW);
    pinMode(PIN_ZERO_CROSS, INPUT);

    // Configuración del ADC1 de 12 bits (0-4095) para Potenciómetro
    analogReadResolution(12);

    // 2. Ángulo inicial por defecto
    set_angulo_disparo_deg(angulo_disparo_deg, false);

    // 3. Adjuntar interrupción ZCD (flanco descendente FALLING)
    attachInterrupt(digitalPinToInterrupt(PIN_ZERO_CROSS), isrCrucePorCero, FALLING);

    // 4. Crear tarea de red FreeRTOS asignada al Core 0
    xTaskCreatePinnedToCore(
        TaskComunicaciones,
        "TaskComunicaciones",
        10000,
        NULL,
        1,
        &ManejadorOTA,
        0 // Core 0
    );

    Serial.println("[Core 1] Interrupción ZCD en GPIO 4 activa.");
    Serial.println("--- Sistema listo (Potenciómetro en GPIO 36 / Disparo en GPIO 16 / UDP Puerto 8888) ---");
}

// ==========================================
// 8. BUCLE PRINCIPAL DE POTENCIA (Arduino Standard Loop)
// ==========================================
void loop() {
    // 1. Lectura del potenciómetro en ADC (GPIO 36) si el modo ADC está activo
    // Solo actualizamos si NO estamos en medio de la espera del disparo
    if (modo_adc_potenciometro && !cruceDetectado) {
        int lecturaADC = analogRead(PIN_POTENTIOMETER);
        // Mapeo ADC (0-4095) a grados seguros (5° - 175°)
        float angulo_pot = (float)map(lecturaADC, 0, 4095, 5, 175);
        set_angulo_disparo_deg(angulo_pot, true);
    }

    // 2. Ejecución del disparo de compuerta en sincronía con el cruce por cero
    if (cruceDetectado) {
        unsigned long retardo_us;
        portENTER_CRITICAL(&timerMux);
        retardo_us = retardo_disparo_us;
        portEXIT_CRITICAL(&timerMux);

        unsigned long tiempoActual = micros();
        if (tiempoActual - tiempoUltimoCruce >= retardo_us) {
            digitalWrite(PIN_DISPARO, HIGH);
            delayMicroseconds(ANCHO_PULSO_DISPARO_US);
            digitalWrite(PIN_DISPARO, LOW);

            cruceDetectado = false;
        }
    }

    // 3. Comandos vía Serial para pruebas locales manuales
    if (Serial.available() > 0) {
        String inputStr = Serial.readStringUntil('\n');
        inputStr.trim();
        if (inputStr.equalsIgnoreCase("ADC")) {
            portENTER_CRITICAL(&timerMux);
            modo_adc_potenciometro = true;
            portEXIT_CRITICAL(&timerMux);
            Serial.println("[Serial] Modo ADC Potenciómetro ACTIVADO.");
        } else {
            float nuevo_angulo = inputStr.toFloat();
            if (nuevo_angulo >= 0.0f && nuevo_angulo <= 180.0f) {
                set_angulo_disparo_deg(nuevo_angulo, false);
                Serial.printf("[Serial] Nuevo ángulo fijado: %.1f°\n", nuevo_angulo);
            }
        }
    }
}
