#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiUdp.h>

/**
 * ============================================================================
 * Proyecto: InverRect-Monitor - Control de Rectificador con ESP32
 * Archivo: esp32_firmware/esp32_firmware.ino (Sketch compatible con Arduino IDE)
 * Carpeta: esp32_firmware/
 * Arquitectura: Dual-Core (FreeRTOS) + Disparo Único (Media Onda / Onda Completa)
 *               + Adquisición Síncrona ADC (Tensión D35 / Corriente ACS712 D34) + SCADA UDP
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
#define PIN_ZERO_CROSS     4   // Entrada digital ZCD: Cruce por cero (Optoacoplador en GPIO 4)
#define PIN_DISPARO        16  // Salida digital Disparo: Circuito único para ambos tiristores (MOC3021 en GPIO 16)
#define PIN_SELECTOR_ONDA  25  // Entrada digital Selector de Onda HW con Pull-Up (HIGH = Onda Completa, LOW = Media Onda)
#define PIN_ADC_CORRIENTE  34  // Entrada analógica Corriente de Salida: Sensor ACS712 con divisor resistivo (ADC1_CH6 en GPIO 34)
#define PIN_ADC_TENSION    35  // Entrada analógica Tensión de Salida: Divisor resistivo de salida (ADC1_CH7 en GPIO 35)
#define PIN_POTENTIOMETER  36  // Entrada analógica Potenciómetro de ángulo alfa: (ADC1_CH0 / VP en GPIO 36)

// Parámetros temporales y de sincronismo de potencia
#define FRECUENCIA_RED_HZ             50.0f
#define PERIODO_SEMICICLO_DEFAULT_US  10000 // 10 ms (180°) por semiciclo en red de 50 Hz
#define ANCHO_PULSO_DISPARO_US        50    // Ancho del pulso de compuerta en us (50 us)
#define ADELANTO_COMPENSACION_US      100   // Compensación de retardo físico del optoacoplador (100 us)
#define FILTRO_DEBOUNCE_ZCD_US        6000  // Antirrebote ZCD (>6 ms descarta ruidos permitiendo cada semiciclo de 10 ms)

// ==========================================
// 1.1 PARÁMETROS DE CALIBRACIÓN Y ESCALADO DEL ADC (D35 Y D34)
// ==========================================
#define NUM_MUESTRAS_CICLO        64        // Muestras por ciclo completo (360° = 20 ms) enviadas a la interfaz
#define ADC_VREF_V                3.3f      // Tensión de referencia del ADC del ESP32 (3.3 V)
#define ADC_MAX_COUNTS            4095.0f   // Resolución de 12 bits (0 - 4095)

// Divisor resistivo de Tensión en D35 (En PCB: R10 = 330k para 220V o R9 = 18k para 12V, con R11 = 1k)
#define R_DIV_TENSION_SUP_OHM     330000.0f // Resistencia superior del divisor de tensión (330 kOhm en 220V / 18 kOhm en 12V)
#define R_DIV_TENSION_INF_OHM     1000.0f   // Resistencia inferior del divisor de tensión (R11 = 1 kOhm)
#define FACTOR_ESCALA_TENSION     ((R_DIV_TENSION_SUP_OHM + R_DIV_TENSION_INF_OHM) / R_DIV_TENSION_INF_OHM)
#define OFFSET_TENSION_ADC_V      1.65f     // Tierra virtual V_1.65 (1.65 V, o 0.0f si el jumper referencia a GND)

// Sensor de Corriente ACS712 y su divisor resistivo en D34 (En PCB: R16 = 1k en serie y R15 = 2k a GND)
#define R_DIV_ACS_SERIE_OHM       1000.0f   // Resistencia superior del divisor del ACS712 (R16 = 1 kOhm)
#define R_DIV_ACS_GND_OHM         2000.0f   // Resistencia inferior a GND del divisor del ACS712 (R15 = 2 kOhm)
#define FACTOR_DIVISOR_ACS        ((R_DIV_ACS_SERIE_OHM + R_DIV_ACS_GND_OHM) / R_DIV_ACS_GND_OHM) // Factor 1.5
#define ACS712_OFFSET_CERO_V      2.50f     // Tensión de salida del ACS712 para 0 A (VCC / 2 = 2.50 V)
#define ACS712_SENSIBILIDAD_V_A   0.100f    // Sensibilidad del ACS712: 0.100 V/A (20A), 0.185 V/A (5A), 0.066 V/A (30A)

// ==========================================
// 1.2 CONFIGURACIÓN DE RED Y COMUNICACIONES (CORE 0)
// ==========================================
const char *WIFI_SSID     = "TU_SSID_AQUI";     // Configurar con el SSID de la red local
const char *WIFI_PASSWORD = "TU_PASSWORD_AQUI"; // Configurar con la clave de la red local
const uint16_t UDP_PORT   = 8888;               // Puerto UDP local de escucha

WiFiUDP udp;

// Spinlock / MUX para acceso atómico entre Core 0 (Red UDP) y Core 1 (Potencia/ADC/ISR)
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// Manejador de la Tarea FreeRTOS de red
TaskHandle_t ManejadorOTA = NULL;

// ==========================================
// 2. VARIABLES GLOBALES VOLÁTILES Y BUFFERS DE TELEMETRÍA
// ==========================================
volatile float angulo_disparo_deg = 90.0f;           // Ángulo de disparo alfa en grados (5.0° a 175.0°)
volatile uint32_t retardo_disparo_us = 5000;         // Retardo equivalente en microsegundos dentro del semiciclo
volatile unsigned long tiempoUltimoCruce = 0;        // Timestamp (micros) del último cruce ZCD válido
volatile unsigned long periodoSemiciclo = PERIODO_SEMICICLO_DEFAULT_US; // Duración real medida del semiciclo (180°)
volatile bool cruceDetectado = false;                // Bandera de disparo habilitado para el semiciclo actual
volatile bool semicicloIntercalado = false;          // Alterna en cada cruce por cero para disparo de media onda
volatile bool nuevoCicloMuestreoZCD = false;         // Indica el inicio de un ciclo completo (0° a 360°) para sincronizar el ADC
volatile unsigned long timestampInicioCiclo = 0;     // Timestamp del cruce de inicio de ciclo completo

// Banderas de modo de operación y prioridad Interfaz vs Hardware
volatile bool control_interfaz_activo = false;       // true = Obedece a la interfaz Wi-Fi/UDP; false = Evalúa hardware local
volatile bool modo_adc_potenciometro = true;         // true = Ángulo por Potenciómetro ADC; false = Ángulo fijo por UDP
volatile bool modo_onda_completa = true;             // true = Onda Completa (todos los cruces); false = Media Onda (intercalado)

// Buffers de adquisición (Core 1) y de transmisión UDP (compartidos con Core 0)
float buffer_captura_v[NUM_MUESTRAS_CICLO];
float buffer_captura_i[NUM_MUESTRAS_CICLO];
float buffer_listo_v[NUM_MUESTRAS_CICLO];
float buffer_listo_i[NUM_MUESTRAS_CICLO];

// ==========================================
// 3. PROTOTIPOS DE FUNCIONES
// ==========================================
inline uint32_t calcular_retardo_us(float grados, unsigned long periodo_semiciclo_us);
inline float convertir_adc_a_tension(int adc_raw);
inline float convertir_adc_a_corriente(int adc_raw);
void actualizar_parametros_disparo(float grados, bool usar_adc, bool onda_completa, bool desde_interfaz);
void IRAM_ATTR isrCrucePorCero();
void setup_red();
void TaskComunicaciones(void *pvParameters);

// ==========================================
// 4. FUNCIONES DE CÁLCULO, ESCALADO Y CONTROL
// ==========================================

/**
 * Convierte un ángulo en grados (5.0° - 175.0°) a retardo en microsegundos (us)
 * utilizando el periodo de semiciclo (180°) medido dinámicamente.
 */
inline uint32_t calcular_retardo_us(float grados, unsigned long periodo_semiciclo_us) {
    if (grados < 5.0f) grados = 5.0f;
    if (grados > 175.0f) grados = 175.0f;

    float us_base = (periodo_semiciclo_us * grados) / 180.0f;
    if (us_base > ADELANTO_COMPENSACION_US) {
        return (uint32_t)(us_base - ADELANTO_COMPENSACION_US);
    }
    return 10; // Retardo mínimo seguro en us
}

/**
 * Convierte la lectura cruda del ADC en D35 (0-4095) a tensión real de salida en Voltios (V),
 * considerando la referencia de tierra virtual y la relación del divisor resistivo.
 */
inline float convertir_adc_a_tension(int adc_raw) {
    float v_pin = (adc_raw / ADC_MAX_COUNTS) * ADC_VREF_V;
    return (v_pin - OFFSET_TENSION_ADC_V) * FACTOR_ESCALA_TENSION;
}

/**
 * Convierte la lectura cruda del ADC en D34 (0-4095) a corriente real de salida en Amperios (A),
 * compensando primero el divisor resistivo (R16/R15) y luego el offset y sensibilidad del ACS712.
 */
inline float convertir_adc_a_corriente(int adc_raw) {
    float v_pin = (adc_raw / ADC_MAX_COUNTS) * ADC_VREF_V;
    float v_acs = v_pin * FACTOR_DIVISOR_ACS;
    return (v_acs - ACS712_OFFSET_CERO_V) / ACS712_SENSIBILIDAD_V_A;
}

/**
 * Actualiza de forma atómica el ángulo de disparo, el modo de onda (Media Onda / Onda Completa)
 * y la fuente de control (Interfaz SCADA vs Hardware local).
 */
void actualizar_parametros_disparo(float grados, bool usar_adc, bool onda_completa, bool desde_interfaz) {
    if (grados < 5.0f) grados = 5.0f;
    if (grados > 175.0f) grados = 175.0f;

    portENTER_CRITICAL(&timerMux);
    angulo_disparo_deg = grados;
    retardo_disparo_us = calcular_retardo_us(grados, periodoSemiciclo);
    modo_adc_potenciometro = usar_adc;
    modo_onda_completa = onda_completa;
    control_interfaz_activo = desde_interfaz;
    portEXIT_CRITICAL(&timerMux);
}

// ==========================================
// 5. RUTINA DE INTERRUPCIÓN ZCD (CORE 1 - IRAM)
// ==========================================

/**
 * ISR: Cruce por Cero (Zero Cross Detection)
 * Se ejecuta en cada cruce por cero de la red (cada semiciclo de 180°, ~10 ms a 50 Hz).
 * - En modo Onda Completa: habilita el disparo en TODOS los cruces por cero.
 * - En modo Media Onda: habilita el disparo de manera INTERCALADA (un semiciclo sí, otro no).
 */
void IRAM_ATTR isrCrucePorCero() {
    unsigned long tiempoActual = micros();
    unsigned long tiempoMedido = tiempoActual - tiempoUltimoCruce;

    // Filtro temporal antirrebote: descarta pulsos espurios menores a 6 ms
    if (tiempoMedido > FILTRO_DEBOUNCE_ZCD_US) {
        if (tiempoMedido <= 15000) {
            periodoSemiciclo = tiempoMedido;
        } else if (tiempoMedido <= 25000) {
            periodoSemiciclo = tiempoMedido / 2;
        } else {
            periodoSemiciclo = PERIODO_SEMICICLO_DEFAULT_US;
        }

        tiempoUltimoCruce = tiempoActual;
        semicicloIntercalado = !semicicloIntercalado;

        // Sincronizar el inicio de muestreo de ciclo completo (360°) en cada semiciclo par
        if (semicicloIntercalado) {
            timestampInicioCiclo = tiempoActual;
            nuevoCicloMuestreoZCD = true;
        }

        // Si es Onda Completa dispara en todos los cruces; si es Media Onda dispara intercalado
        if (modo_onda_completa || semicicloIntercalado) {
            cruceDetectado = true;
        }
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
        Serial.println("\n[Core 0] Aviso: Sin conexión Wi-Fi (Timeout 15s). Evaluando controles de hardware local.");
        portENTER_CRITICAL(&timerMux);
        control_interfaz_activo = false;
        modo_adc_potenciometro = true;
        portEXIT_CRITICAL(&timerMux);
    }
}

void TaskComunicaciones(void *pvParameters) {
    (void)pvParameters;
    setup_red();

    char packetBuffer[64];
    static char txBuffer[1400]; // Buffer para empaquetar las 64 muestras de tensión y corriente en un datagrama UDP
    float copia_v[NUM_MUESTRAS_CICLO];
    float copia_i[NUM_MUESTRAS_CICLO];

    uint32_t ultimo_reintento_wifi = 0;
    bool servicios_inicializados = (WiFi.status() == WL_CONNECTED);

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            // Ante caída o error de conexión inalámbrica, el sistema obedece inmediatamente al hardware físico
            if (servicios_inicializados || control_interfaz_activo) {
                portENTER_CRITICAL(&timerMux);
                control_interfaz_activo = false;
                modo_adc_potenciometro = true;
                portEXIT_CRITICAL(&timerMux);
                servicios_inicializados = false;
                Serial.println("[Core 0] Conexión Wi-Fi perdida: Conmutando automáticamente a control por Hardware (Potenciómetro + Pin D25).");
            }

            uint32_t ahora = millis();
            if (ahora - ultimo_reintento_wifi > 10000) {
                ultimo_reintento_wifi = ahora;
                Serial.println("[Core 0] Reintentando conexión Wi-Fi...");
                WiFi.reconnect();
            }
        } else {
            if (!servicios_inicializados) {
                Serial.println("\n[Core 0] ¡Wi-Fi reconectado!");
                Serial.print("[Core 0] Dirección IP asignada: ");
                Serial.println(WiFi.localIP());
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
                    mensaje.toUpperCase();

                    // Caso A: Solicitud de lectura de muestras ADC de Tensión (D35) y Corriente (D34)
                    if (mensaje.startsWith("GET_ADC") || mensaje.startsWith("LEER_ADC")) {
                        float alfa_snap;
                        bool onda_snap;

                        portENTER_CRITICAL(&timerMux);
                        alfa_snap = angulo_disparo_deg;
                        onda_snap = modo_onda_completa;
                        for (int k = 0; k < NUM_MUESTRAS_CICLO; k++) {
                            copia_v[k] = buffer_listo_v[k];
                            copia_i[k] = buffer_listo_i[k];
                        }
                        portEXIT_CRITICAL(&timerMux);

                        const char *str_onda = onda_snap ? "COMPLETA" : "MEDIA";
                        int pos = snprintf(
                            txBuffer,
                            sizeof(txBuffer),
                            "ADC_DATA:ALFA:%.1f;ONDA:%s;V:",
                            alfa_snap,
                            str_onda
                        );

                        for (int k = 0; k < NUM_MUESTRAS_CICLO && pos < (int)sizeof(txBuffer) - 24; k++) {
                            pos += snprintf(
                                txBuffer + pos,
                                sizeof(txBuffer) - pos,
                                (k < NUM_MUESTRAS_CICLO - 1) ? "%.2f," : "%.2f",
                                copia_v[k]
                            );
                        }

                        pos += snprintf(txBuffer + pos, sizeof(txBuffer) - pos, ";I:");

                        for (int k = 0; k < NUM_MUESTRAS_CICLO && pos < (int)sizeof(txBuffer) - 16; k++) {
                            pos += snprintf(
                                txBuffer + pos,
                                sizeof(txBuffer) - pos,
                                (k < NUM_MUESTRAS_CICLO - 1) ? "%.3f," : "%.3f\n",
                                copia_i[k]
                            );
                        }

                        udp.beginPacket(udp.remoteIP(), udp.remotePort());
                        udp.write((const uint8_t *)txBuffer, pos);
                        udp.endPacket();
                    }
                    // Caso B: Comandos de configuración de Ángulo y Modo de Onda
                    else {
                        portENTER_CRITICAL(&timerMux);
                        bool onda_completa_actual = modo_onda_completa;
                        bool usar_adc_actual = modo_adc_potenciometro;
                        float angulo_actual = angulo_disparo_deg;
                        portEXIT_CRITICAL(&timerMux);

                        bool comando_valido = false;
                        bool error_rango = false;
                        float alfa_recibido = angulo_actual;

                        // 1. Evaluar selector de onda si está presente en el mensaje ("ONDA:COMPLETA" o "ONDA:MEDIA")
                        if (mensaje.indexOf("ONDA:COMPLETA") >= 0) {
                            onda_completa_actual = true;
                            comando_valido = true;
                        } else if (mensaje.indexOf("ONDA:MEDIA") >= 0) {
                            onda_completa_actual = false;
                            comando_valido = true;
                        }

                        // 2. Evaluar comando de ángulo o modo ADC ("ALFA:ADC", "MODO:ADC" o "ALFA:xx.x")
                        if (mensaje.indexOf("ALFA:ADC") >= 0 || mensaje.indexOf("MODO:ADC") >= 0) {
                            usar_adc_actual = true;
                            comando_valido = true;
                        } else if (mensaje.startsWith("ALFA:")) {
                            int idx_fin = mensaje.indexOf(';');
                            String sub_alfa = (idx_fin > 5) ? mensaje.substring(5, idx_fin) : mensaje.substring(5);
                            alfa_recibido = sub_alfa.toFloat();

                            if (alfa_recibido >= 0.0f && alfa_recibido <= 180.0f) {
                                usar_adc_actual = false;
                                angulo_actual = alfa_recibido;
                                comando_valido = true;
                            } else {
                                error_rango = true;
                            }
                        }

                        // 3. Aplicar cambios y enviar confirmación (ACK/NACK) a la interfaz gráfica
                        if (error_rango) {
                            Serial.printf("[Core 0][UDP] Ángulo fuera de rango (0-180°): %.1f°\n", alfa_recibido);
                            udp.beginPacket(udp.remoteIP(), udp.remotePort());
                            udp.printf("NACK:OUT_OF_RANGE:%.1f\n", alfa_recibido);
                            udp.endPacket();
                        } else if (comando_valido) {
                            actualizar_parametros_disparo(angulo_actual, usar_adc_actual, onda_completa_actual, true);
                            const char *str_onda = onda_completa_actual ? "COMPLETA" : "MEDIA";

                            if (usar_adc_actual) {
                                Serial.printf("[Core 0][UDP] Modo ADC activo | Onda: %s\n", str_onda);
                                udp.beginPacket(udp.remoteIP(), udp.remotePort());
                                udp.printf("ACK:ALFA:ADC:%.1f;ONDA:%s\n", angulo_disparo_deg, str_onda);
                                udp.endPacket();
                            } else {
                                Serial.printf("[Core 0][UDP] Comando aplicado: ALFA = %.1f° | Onda: %s\n", angulo_actual, str_onda);
                                udp.beginPacket(udp.remoteIP(), udp.remotePort());
                                udp.printf("ACK:ALFA:%.1f;ONDA:%s\n", angulo_actual, str_onda);
                                udp.endPacket();
                            }
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

// ==========================================
// 7. SETUP E INICIALIZACIÓN PRINCIPAL (Arduino Standard Entry Point)
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- InverRect-Monitor: Inicializando Firmware ESP32 (Disparo Único + Telemetría ADC D35/D34) ---");

    // 1. Configuración de Pines GPIO
    pinMode(PIN_DISPARO, OUTPUT);
    digitalWrite(PIN_DISPARO, LOW);
    pinMode(PIN_ZERO_CROSS, INPUT);
    pinMode(PIN_SELECTOR_ONDA, INPUT_PULLUP); // Pull-up en D25 (HIGH = Onda Completa, LOW = Media Onda)
    pinMode(PIN_ADC_TENSION, INPUT);          // D35: Divisor resistivo de tensión de salida
    pinMode(PIN_ADC_CORRIENTE, INPUT);        // D34: Salida de sensor de corriente ACS712 con divisor resistivo
    pinMode(PIN_POTENTIOMETER, INPUT);        // D36: Potenciómetro para ángulo de disparo

    // Configuración del ADC1 de 12 bits (0-4095) y atenuación de 11 dB (rango completo hasta 3.3 V)
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    // 2. Evaluar estado inicial del hardware local antes de establecer conexión
    bool onda_hw_inicial = (digitalRead(PIN_SELECTOR_ONDA) == HIGH);
    int adc_inicial = analogRead(PIN_POTENTIOMETER);
    float angulo_inicial = (float)map(adc_inicial, 0, 4095, 5, 175);
    actualizar_parametros_disparo(angulo_inicial, true, onda_hw_inicial, false);

    // 3. Adjuntar interrupción ZCD en ambos flancos (CHANGE) para detectar todos los cruces por cero (cada 10 ms)
    attachInterrupt(digitalPinToInterrupt(PIN_ZERO_CROSS), isrCrucePorCero, CHANGE);

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

    Serial.println("[Core 1] Interrupción ZCD en GPIO 4 y muestreo ADC en D35 (V) / D34 (I) activos.");
    Serial.println("--- Sistema listo (Disparo GPIO 16 / Potenciómetro GPIO 36 / Selector Onda GPIO 25) ---");
}

// ==========================================
// 8. BUCLE PRINCIPAL DE POTENCIA Y ADQUISICIÓN SÍNCRONA ADC (CORE 1)
// ==========================================
void loop() {
    static int indiceMuestra = NUM_MUESTRAS_CICLO; // Inicia completado a la espera del primer ciclo
    static unsigned long tInicioMuestreo = 0;
    static unsigned long pasoMuestreo_us = (PERIODO_SEMICICLO_DEFAULT_US * 2) / NUM_MUESTRAS_CICLO;

    // 1. PRIORIDAD MÁXIMA: Ejecución del disparo de compuerta en sincronía con el cruce por cero
    bool proximo_a_disparar = false;
    if (cruceDetectado) {
        unsigned long retardo_us;
        portENTER_CRITICAL(&timerMux);
        retardo_us = retardo_disparo_us;
        portEXIT_CRITICAL(&timerMux);

        unsigned long transcurrido = micros() - tiempoUltimoCruce;
        if (transcurrido >= retardo_us) {
            digitalWrite(PIN_DISPARO, HIGH);
            delayMicroseconds(ANCHO_PULSO_DISPARO_US);
            digitalWrite(PIN_DISPARO, LOW);
            cruceDetectado = false;
        } else if ((retardo_us - transcurrido) < 80) {
            // Si faltan menos de 80 us para el disparo, posponer lecturas analógicas para evitar jitter
            proximo_a_disparar = true;
        }
    }

    if (proximo_a_disparar) {
        return;
    }

    // 2. Sincronización de inicio de ventana de muestreo de ciclo completo (360° = 2 semiciclos)
    unsigned long ahora_us = micros();
    if (indiceMuestra >= NUM_MUESTRAS_CICLO) {
        if (nuevoCicloMuestreoZCD) {
            portENTER_CRITICAL(&timerMux);
            nuevoCicloMuestreoZCD = false;
            tInicioMuestreo = timestampInicioCiclo;
            pasoMuestreo_us = (periodoSemiciclo * 2) / NUM_MUESTRAS_CICLO;
            portEXIT_CRITICAL(&timerMux);
            if (pasoMuestreo_us < 100) pasoMuestreo_us = 312;
            indiceMuestra = 0;
        } else if (ahora_us - tiempoUltimoCruce > 40000) {
            // Modo autónomo (free-running a 50 Hz) si no hay señal de red en el detector ZCD
            tInicioMuestreo = ahora_us;
            pasoMuestreo_us = (PERIODO_SEMICICLO_DEFAULT_US * 2) / NUM_MUESTRAS_CICLO;
            indiceMuestra = 0;
        }
    }

    // 3. Adquisición equidistante de Tensión (D35) y Corriente (D34 - ACS712)
    if (indiceMuestra < NUM_MUESTRAS_CICLO) {
        if ((ahora_us - tInicioMuestreo) >= ((unsigned long)indiceMuestra * pasoMuestreo_us)) {
            int raw_v = analogRead(PIN_ADC_TENSION);
            int raw_i = analogRead(PIN_ADC_CORRIENTE);

            buffer_captura_v[indiceMuestra] = convertir_adc_a_tension(raw_v);
            buffer_captura_i[indiceMuestra] = convertir_adc_a_corriente(raw_i);
            indiceMuestra++;

            // Al completar las N muestras del ciclo completo, publicar de forma atómica hacia el buffer de red
            if (indiceMuestra >= NUM_MUESTRAS_CICLO) {
                portENTER_CRITICAL(&timerMux);
                for (int k = 0; k < NUM_MUESTRAS_CICLO; k++) {
                    buffer_listo_v[k] = buffer_captura_v[k];
                    buffer_listo_i[k] = buffer_captura_i[k];
                }
                portEXIT_CRITICAL(&timerMux);
            }
        }
    }

    // 4. Evaluación de controles (Potenciómetro D36 y Selector de Onda D25) fuera de la espera del disparo
    if (!cruceDetectado) {
        bool interfaz_activa;
        bool usar_adc;
        bool onda_actual;
        float angulo_actual;

        portENTER_CRITICAL(&timerMux);
        interfaz_activa = control_interfaz_activo;
        usar_adc = modo_adc_potenciometro;
        onda_actual = modo_onda_completa;
        angulo_actual = angulo_disparo_deg;
        portEXIT_CRITICAL(&timerMux);

        // Si NO hay conexión activa con la interfaz, evaluar directamente el hardware (Pin D25 y Potenciómetro D36)
        if (!interfaz_activa) {
            onda_actual = (digitalRead(PIN_SELECTOR_ONDA) == HIGH); // HIGH = Onda Completa, LOW = Media Onda
            int lecturaADC = analogRead(PIN_POTENTIOMETER);
            angulo_actual = (float)map(lecturaADC, 0, 4095, 5, 175);
            actualizar_parametros_disparo(angulo_actual, true, onda_actual, false);
        }
        // Si hay conexión con la interfaz, obedecer el modo de onda de la interfaz y leer potenciómetro si se seleccionó modo ADC
        else if (usar_adc) {
            int lecturaADC = analogRead(PIN_POTENTIOMETER);
            angulo_actual = (float)map(lecturaADC, 0, 4095, 5, 175);
            actualizar_parametros_disparo(angulo_actual, true, onda_actual, true);
        } else {
            portENTER_CRITICAL(&timerMux);
            retardo_disparo_us = calcular_retardo_us(angulo_disparo_deg, periodoSemiciclo);
            portEXIT_CRITICAL(&timerMux);
        }
    }

    // 5. Comandos vía Serial para pruebas locales manuales
    if (Serial.available() > 0) {
        String inputStr = Serial.readStringUntil('\n');
        inputStr.trim();
        inputStr.toUpperCase();

        if (inputStr.equals("ADC")) {
            actualizar_parametros_disparo(angulo_disparo_deg, true, modo_onda_completa, true);
            Serial.println("[Serial] Modo ADC Potenciómetro ACTIVADO.");
        } else if (inputStr.equals("COMPLETA")) {
            actualizar_parametros_disparo(angulo_disparo_deg, modo_adc_potenciometro, true, true);
            Serial.println("[Serial] Salida configurada en ONDA COMPLETA (todos los cruces).");
        } else if (inputStr.equals("MEDIA")) {
            actualizar_parametros_disparo(angulo_disparo_deg, modo_adc_potenciometro, false, true);
            Serial.println("[Serial] Salida configurada en MEDIA ONDA (disparo intercalado).");
        } else if (inputStr.equals("HW")) {
            actualizar_parametros_disparo(angulo_disparo_deg, true, modo_onda_completa, false);
            Serial.println("[Serial] Control cedido íntegramente al HARDWARE local.");
        } else {
            float nuevo_angulo = inputStr.toFloat();
            if (nuevo_angulo >= 0.0f && nuevo_angulo <= 180.0f) {
                actualizar_parametros_disparo(nuevo_angulo, false, modo_onda_completa, true);
                Serial.printf("[Serial] Nuevo ángulo fijado: %.1f°\n", nuevo_angulo);
            }
        }
    }
}
