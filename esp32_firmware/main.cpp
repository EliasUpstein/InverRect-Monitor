#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

/**
 * ============================================================================
 * Proyecto: InverRect-Monitor - Control de Rectificador con ESP32
 * Módulo: esp32_firmware/main.cpp
 * Arquitectura: Dual-Core (FreeRTOS) + Temporizadores de Hardware
 * 
 * Core 1: Control de disparo en tiempo real (Interrupciones y Hardware Timer)
 * Core 0: Tarea de comunicaciones de red (Wi-Fi, UDP, OTA y Telemetría)
 * ============================================================================
 */

// ==========================================
// 1. DEFINICIONES DE HARDWARE Y PINES
// ==========================================
#define PIN_ZERO_CROSS 18  // Entrada digital: Cruce por cero (detector optoacoplado PC817)
#define PIN_DISPARO    19  // Salida digital: Disparo de compuerta (MOC3021 / TRIAC / Tiristor)

// Frecuencia de la red eléctrica de corriente alterna (50 Hz en Argentina / Europa, o 60 Hz)
#define FRECUENCIA_RED_HZ 50.0f 

// Duración de un semiciclo (180°) en microsegundos: T_semi = 1 / (2 * f) * 1e6 = 10000 us a 50 Hz
#define SEMIPERIODO_US    (1000000.0f / (2.0f * FRECUENCIA_RED_HZ))

// ==========================================
// 1.1 CONFIGURACIÓN DE RED Y COMUNICACIONES (CORE 0)
// ==========================================
const char* WIFI_SSID     = "TU_SSID_AQUI";        // Configurar con el SSID de la red local
const char* WIFI_PASSWORD = "TU_PASSWORD_AQUI";    // Configurar con la clave de la red local
const uint16_t UDP_PORT   = 8888;                  // Puerto UDP local de escucha para comandos

WiFiUDP udp;

// Spinlock / MUX para acceso atómico y seguro entre Core 0 (red) y Core 1 (ISR)
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// ==========================================
// 2. VARIABLES GLOBALES VOLÁTILES
// ==========================================
// Variables de control de fase modificables dinámicamente
volatile float angulo_disparo_deg = 90.0f;  // Ángulo de disparo alfa en grados sexagesimales (0.0° a 180.0°)
volatile uint32_t angulo_disparo_us = 5000; // Retardo alfa equivalente en microsegundos (calculado automáticamente)
volatile uint32_t offset_hardware_us = 200; // Compensación de retardo físico del PC817 (200 us por defecto)
volatile uint32_t last_zc_time = 0;        // Timestamp del último cruce por cero válido (filtro de rebotes)

// Puntero al temporizador de hardware del ESP32
hw_timer_t * timer = NULL;

// ==========================================
// 3. FUNCIONES DE CONVERSIÓN Y CONTROL
// ==========================================

/**
 * Convierte un ángulo en grados sexagesimales (0.0° - 180.0°) a tiempo en microsegundos (us).
 * Fórmula: t_us = (alfa_deg / 180.0) * SEMIPERIODO_US
 */
inline uint32_t grados_a_microsegundos(float grados) {
    if (grados < 0.0f) grados = 0.0f;
    if (grados > 180.0f) grados = 180.0f;
    return (uint32_t)((grados / 180.0f) * SEMIPERIODO_US);
}

/**
 * Actualiza el ángulo de disparo a partir de un valor en grados sexagesimales.
 * Aplica el offset de hardware y protege el acceso a memoria con sección crítica.
 */
void set_angulo_disparo_deg(float grados) {
    if (grados < 0.0f) grados = 0.0f;
    if (grados > 180.0f) grados = 180.0f;

    uint32_t us_base = grados_a_microsegundos(grados);
    uint32_t us_final;
    if (us_base > offset_hardware_us) {
        us_final = us_base - offset_hardware_us;
    } else {
        us_final = 10; // Disparo inmediato seguro (evita saltos y no-monotonicidad)
    }
    if (us_final < 10) us_final = 10;

    portENTER_CRITICAL(&timerMux);
    angulo_disparo_deg = grados;
    angulo_disparo_us = us_final;
    portEXIT_CRITICAL(&timerMux);

    Serial.print("[Control] Nuevo ángulo fijado: ");
    Serial.print(angulo_disparo_deg, 1);
    Serial.print("° -> Retardo timer (con offset): ");
    Serial.print(angulo_disparo_us);
    Serial.println(" us");
}

// ==========================================
// 4. RUTINAS DE INTERRUPCIÓN (CORE 1 - IRAM)
// ==========================================

/**
 * ISR: Cruce por Cero (Zero Cross Detection)
 * Se ejecuta inmediatamente tras cada cruce por cero de la tensión de red.
 * Detiene el timer anterior, obtiene atómicamente el retardo precalculado y reinicia el timer.
 */
void IRAM_ATTR zero_cross_isr() {
    // Filtro temporal antirrebote: ningún cruce legítimo ocurre antes de 7000 us (50 Hz -> 10000 us semiperiodo)
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (now - last_zc_time < 7000) {
        return; // Descartar pico de ruido transitorio
    }
    last_zc_time = now;

    // 1. Detener el temporizador si estaba corriendo y reiniciar contador a cero
    timerStop(timer);
    timerWrite(timer, 0);

    // 2. Lectura atómica y segura del retardo de disparo efectivo (offset ya aplicado)
    portENTER_CRITICAL_ISR(&timerMux);
    uint32_t tiempo_alarma = angulo_disparo_us;
    portEXIT_CRITICAL_ISR(&timerMux);

    // Protección mínima para evitar valores nulos
    if (tiempo_alarma < 10) {
        tiempo_alarma = 10;
    }

    // 3. Configurar alarma y reiniciar temporizador en modo disparo único (autoreload = false)
    timerAlarmWrite(timer, tiempo_alarma, false);
    timerAlarmEnable(timer);
    timerStart(timer);
}

/**
 * ISR: Temporizador de Hardware
 * Se dispara cuando se cumple el tiempo correspondiente al ángulo alfa.
 * Emite un pulso corto de compuerta para activar el tiristor/TRIAC.
 */
void IRAM_ATTR timer_isr() {
    // Generar pulso de disparo (Gate trigger)
    digitalWrite(PIN_DISPARO, HIGH);
    
    // Retardo breve para asegurar enganche del tiristor (50 microsegundos en IRAM)
    ets_delay_us(50);
    
    digitalWrite(PIN_DISPARO, LOW);
}

// ==========================================
// 5. CONFIGURACIÓN DE RED Y TAREAS (CORE 0)
// ==========================================

/**
 * Conecta el ESP32 a la red Wi-Fi e inicializa los servicios de ArduinoOTA y UDP.
 * Cuenta con timeout de 15 segundos para no bloquear el sistema si no hay red disponible.
 */
void setup_red() {
    Serial.println("[Core 0] Iniciando conexión Wi-Fi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[Core 0] Conectando a ");
    Serial.print(WIFI_SSID);

    int intentos = 0;
    const int max_intentos = 30; // 30 * 500 ms = 15 segundos
    while (WiFi.status() != WL_CONNECTED && intentos < max_intentos) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        intentos++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[Core 0] ¡Wi-Fi conectado con éxito!");
        Serial.print("[Core 0] Dirección IP asignada: ");
        Serial.println(WiFi.localIP());

        // Configuración del servicio ArduinoOTA (Flasheo inalámbrico)
        ArduinoOTA.setHostname("InverRect-ESP32");
        ArduinoOTA.onStart([]() {
            String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
            Serial.println("[OTA] Inicio de actualización: " + type);
        });
        ArduinoOTA.onEnd([]() {
            Serial.println("\n[OTA] Actualización completada con éxito. Reiniciando...");
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            Serial.printf("[OTA] Progreso: %u%%\r", (progress / (total / 100)));
        });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("[OTA] Error[%u]\n", error);
        });
        ArduinoOTA.begin();
        Serial.println("[Core 0] Servicio ArduinoOTA inicializado y a la escucha.");

        // Inicialización del servidor UDP local
        udp.begin(UDP_PORT);
        Serial.printf("[Core 0] Servidor UDP a la escucha en el puerto %u.\n", UDP_PORT);
    } else {
        Serial.println("\n[Core 0] Aviso: Timeout Wi-Fi (15s). Continuando en modo local/Serial.");
    }
}

/**
 * Tarea de Comunicaciones, Red y Telemetría (FreeRTOS)
 * Ejecuta en el Core 0 de manera desacoplada para no afectar el timing crítico del Core 1.
 */
void TaskComunicaciones(void *pvParameters) {
    (void) pvParameters;

    // 1. Inicialización de red Wi-Fi, OTA y UDP en Core 0
    setup_red();

    char packetBuffer[64];
    uint32_t ultimo_reintento_wifi = 0;
    bool servicios_inicializados = (WiFi.status() == WL_CONNECTED);

    for (;;) {
        // Reconexión automática periódica si se pierde la conexión Wi-Fi
        if (WiFi.status() != WL_CONNECTED) {
            servicios_inicializados = false;
            uint32_t ahora = (uint32_t)millis();
            if (ahora - ultimo_reintento_wifi > 10000) { // Reintentar cada 10 segundos
                ultimo_reintento_wifi = ahora;
                Serial.println("[Core 0] Reintentando conexión Wi-Fi...");
                WiFi.reconnect();
            }
        } else {
            // Inicializar servicios si nos conectamos tras un timeout previo
            if (!servicios_inicializados) {
                Serial.println("\n[Core 0] ¡Wi-Fi conectado con éxito tras reconexión!");
                Serial.print("[Core 0] Dirección IP asignada: ");
                Serial.println(WiFi.localIP());
                ArduinoOTA.begin();
                udp.begin(UDP_PORT);
                servicios_inicializados = true;
            }

            // 2. Escucha obligatoria de flasheo inalámbrico OTA en cada iteración
            ArduinoOTA.handle();

            // 3. Lectura y procesamiento de paquetes UDP entrantes
            int packetSize = udp.parsePacket();
            if (packetSize > 0) {
                int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
                if (len > 0) {
                    packetBuffer[len] = '\0';
                    String mensaje = String(packetBuffer);
                    mensaje.trim();

                    // El protocolo espera un string con el formato "ALFA:xx.x" (ej. "ALFA:60.5")
                    if (mensaje.startsWith("ALFA:")) {
                        float alfa = mensaje.substring(5).toFloat();

                        // Validar rango físico seguro (0° a 180°)
                        if (alfa >= 0.0f && alfa <= 180.0f) {
                            // Actualización invocando directamente a la función de control
                            set_angulo_disparo_deg(alfa);
                            Serial.printf("[Core 0][UDP] Comando aplicado: ALFA = %.1f°\n", alfa);

                            // Responder con acuse de recibo (ACK) al remitente para verificar conexión bidireccional
                            udp.beginPacket(udp.remoteIP(), udp.remotePort());
                            udp.printf("ACK:ALFA:%.1f\n", alfa);
                            udp.endPacket();
                        } else {
                            Serial.printf("[Core 0][UDP] Advertencia: Ángulo fuera de rango (0-180°): %.1f°\n", alfa);

                            // Responder con NACK si el ángulo es inválido
                            udp.beginPacket(udp.remoteIP(), udp.remotePort());
                            udp.printf("NACK:OUT_OF_RANGE:%.1f\n", alfa);
                            udp.endPacket();
                        }
                    }
                }
            }
        }

        // Ceder tiempo a la pila Wi-Fi de FreeRTOS con baja latencia
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==========================================
// 6. SETUP Y CONFIGURACIÓN PRINCIPAL
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- InverRect-Monitor: Inicializando Firmware ESP32 ---");

    // 1. Configuración de Pines GPIO
    pinMode(PIN_ZERO_CROSS, INPUT_PULLUP);
    pinMode(PIN_DISPARO, OUTPUT);
    digitalWrite(PIN_DISPARO, LOW);

    // 2. Inicialización del ángulo de disparo en grados sexagesimales (conversión a us)
    set_angulo_disparo_deg(angulo_disparo_deg);

    // 3. Inicialización del Temporizador de Hardware
    // Prescaler = 80 -> Con reloj base de 80 MHz: 80 MHz / 80 = 1 MHz (1 tick = 1 us)
    // Parámetros: timerBegin(numero_timer, prescaler, cuenta_arriba)
    timer = timerBegin(0, 80, true);

    // 4. Vincular interrupción del timer a timer_isr
    timerAttachInterrupt(timer, &timer_isr, true);

    // Configuración inicial de alarma en modo single-shot (autoreload = false)
    timerAlarmWrite(timer, angulo_disparo_us, false);
    timerAlarmEnable(timer);

    // 5. Adjuntar interrupción externa de Cruce por Cero (flanco ascendente)
    attachInterrupt(digitalPinToInterrupt(PIN_ZERO_CROSS), zero_cross_isr, RISING);

    // 6. Creación de la Tarea FreeRTOS asignada al Core 0
    xTaskCreatePinnedToCore(
        TaskComunicaciones,      /* Función que implementa la tarea */
        "TaskComunicaciones",    /* Nombre descriptivo de la tarea */
        8192,                    /* Tamaño de stack asignado (8 KB para Wi-Fi/UDP/OTA) */
        NULL,                    /* Parámetro de entrada a la tarea */
        1,                       /* Prioridad de la tarea (1 = baja/media) */
        NULL,                    /* Handle de la tarea */
        0                        /* Núcleo de ejecución: Core 0 */
    );

    Serial.println("[Core 1] Interrupciones de Cruce por Cero y Temporizador activas.");
    Serial.println("--- Sistema listo para control de fase (Envía grados por Serial para cambiar) ---");
}

// ==========================================
// 6. LOOP PRINCIPAL (CORE 1)
// ==========================================
void loop() {
    // Permite ajustar el ángulo de disparo dinámicamente vía Monitor Serial (ej. enviar '45.0')
    if (Serial.available() > 0) {
        float nuevo_angulo = Serial.parseFloat();
        if (nuevo_angulo >= 0.0f && nuevo_angulo <= 180.0f) {
            set_angulo_disparo_deg(nuevo_angulo);
        }
        // Limpiar caracteres remanentes en el buffer serial
        while (Serial.available() > 0) {
            Serial.read();
        }
    }

    // El Core 1 cede tiempo para no saturar la CPU
    vTaskDelay(pdMS_TO_TICKS(100));
}
