#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiUdp.h>

/**
 * ============================================================================
 * Proyecto: InverRect-Monitor - Control de Rectificador con ESP32
 * Archivo: esp32_firmware/esp32_firmware.ino (Sketch compatible con Arduino
 * IDE) Carpeta: esp32_firmware/ Arquitectura: Dual-Core (FreeRTOS) + Disparo
 * Único (Media Onda / Onda Completa) + ZCD & ADC + SCADA UDP
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
#define PIN_ZERO_CROSS                                                         \
  4 // Entrada digital ZCD: Cruce por cero (Optoacoplador en GPIO 4)
#define PIN_DISPARO                                                            \
  16 // Salida digital Disparo: Circuito único para ambos tiristores (MOC3021 en
     // GPIO 16)
#define PIN_POTENTIOMETER                                                      \
  36 // Entrada analógica Potenciómetro: (ADC1_CH0 / VP en GPIO 36)
#define PIN_SELECTOR_ONDA                                                      \
  25 // Entrada digital Selector de Onda HW con Pull-Up (HIGH = Onda Completa,
     // LOW = Media Onda)

// Parámetros temporales y de sincronismo de potencia
#define FRECUENCIA_RED_HZ 50.0f
#define PERIODO_SEMICICLO_DEFAULT_US                                           \
  10000                           // 10 ms (180°) por semiciclo en red de 50 Hz
#define ANCHO_PULSO_DISPARO_US 50 // Ancho del pulso de compuerta en us (50 us)
#define ADELANTO_COMPENSACION_US                                               \
  100 // Compensación de retardo físico del optoacoplador (100 us)
#define FILTRO_DEBOUNCE_ZCD_US                                                 \
  6000 // Antirrebote ZCD (>6 ms descarta ruidos permitiendo cada semiciclo de
       // 10 ms)

// ==========================================
// 1.1 CONFIGURACIÓN DE RED Y COMUNICACIONES (CORE 0)
// ==========================================
const char *WIFI_SSID =
    "TU_SSID_AQUI"; // Configurar con el SSID de la red local
const char *WIFI_PASSWORD =
    "TU_PASSWORD_AQUI";         // Configurar con la clave de la red local
const uint16_t UDP_PORT = 8888; // Puerto UDP local de escucha

WiFiUDP udp;

// Spinlock / MUX para acceso atómico entre Core 0 (Red UDP) y Core 1
// (Potencia/ADC/ISR)
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

// Manejador de la Tarea FreeRTOS de red
TaskHandle_t ManejadorOTA = NULL;

// ==========================================
// 2. VARIABLES GLOBALES VOLÁTILES
// ==========================================
volatile float angulo_disparo_deg =
    90.0f; // Ángulo de disparo alfa en grados (5.0° a 175.0°)
volatile uint32_t retardo_disparo_us =
    5000; // Retardo equivalente en microsegundos dentro del semiciclo
volatile unsigned long tiempoUltimoCruce =
    0; // Timestamp (micros) del último cruce ZCD válido
volatile unsigned long periodoSemiciclo =
    PERIODO_SEMICICLO_DEFAULT_US; // Duración real medida del semiciclo (180°)
volatile bool cruceDetectado =
    false; // Bandera de disparo habilitado para el semiciclo actual
volatile bool semicicloIntercalado =
    false; // Alterna en cada cruce por cero para disparo de media onda

// Banderas de modo de operación y prioridad Interfaz vs Hardware
volatile bool control_interfaz_activo =
    false; // true = Obedece a la interfaz Wi-Fi/UDP; false = Evalúa hardware
           // local
volatile bool modo_adc_potenciometro =
    true; // true = Ángulo por Potenciómetro ADC; false = Ángulo fijo por UDP
volatile bool modo_onda_completa =
    true; // true = Onda Completa (todos los cruces); false = Media Onda
          // (intercalado)

// ==========================================
// 3. PROTOTIPOS DE FUNCIONES (Para compatibilidad con C++ / Arduino IDE)
// ==========================================
inline uint32_t calcular_retardo_us(float grados,
                                    unsigned long periodo_semiciclo_us);
void actualizar_parametros_disparo(float grados, bool usar_adc,
                                   bool onda_completa, bool desde_interfaz);
void IRAM_ATTR isrCrucePorCero();
void setup_red();
void TaskComunicaciones(void *pvParameters);

// ==========================================
// 4. FUNCIONES DE CÁLCULO Y CONTROL
// ==========================================

/**
 * Convierte un ángulo en grados (5.0° - 175.0°) a retardo en microsegundos (us)
 * utilizando el periodo de semiciclo (180°) medido dinámicamente.
 * Fórmula: retardo_us = ((periodoSemiciclo * angulo) / 180.0) -
 * ADELANTO_COMPENSACION_US
 */
inline uint32_t calcular_retardo_us(float grados,
                                    unsigned long periodo_semiciclo_us) {
  if (grados < 5.0f)
    grados = 5.0f;
  if (grados > 175.0f)
    grados = 175.0f;

  float us_base = (periodo_semiciclo_us * grados) / 180.0f;
  if (us_base > ADELANTO_COMPENSACION_US) {
    return (uint32_t)(us_base - ADELANTO_COMPENSACION_US);
  }
  return 10; // Retardo mínimo seguro en us
}

/**
 * Actualiza de forma atómica el ángulo de disparo, el modo de onda (Media Onda
 * / Onda Completa) y la fuente de control (Interfaz SCADA vs Hardware local).
 */
void actualizar_parametros_disparo(float grados, bool usar_adc,
                                   bool onda_completa, bool desde_interfaz) {
  if (grados < 5.0f)
    grados = 5.0f;
  if (grados > 175.0f)
    grados = 175.0f;

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
 * Se ejecuta en cada cruce por cero de la red (cada semiciclo de 180°, ~10 ms a
 * 50 Hz).
 * - En modo Onda Completa: habilita el disparo en TODOS los cruces por cero.
 * - En modo Media Onda: habilita el disparo de manera INTERCALADA (un semiciclo
 * sí, otro no).
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

    // Si es Onda Completa dispara en todos los cruces; si es Media Onda dispara
    // intercalado
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
      // 1. Desvincular interrupción ZCD para prevenir disparos erráticos
      // durante el flasheo
      detachInterrupt(digitalPinToInterrupt(PIN_ZERO_CROSS));
      // 2. Apagar etapa de potencia por seguridad
      digitalWrite(PIN_DISPARO, LOW);
      Serial.println("[OTA] ¡Inicio de flasheo inalámbrico! Interrupciones de "
                     "potencia suspendidas.");
    });

    ArduinoOTA.onEnd([]() {
      Serial.println(
          "\n[OTA] Actualización completada con éxito. Reiniciando...");
    });

    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("[OTA] Error[%u] al actualizar.\n", error);
    });

    ArduinoOTA.begin();
    udp.begin(UDP_PORT);
    Serial.printf("[Core 0] Servidor UDP a la escucha en el puerto %u.\n",
                  UDP_PORT);
  } else {
    Serial.println("\n[Core 0] Aviso: Sin conexión Wi-Fi (Timeout 15s). "
                   "Evaluando controles de hardware local.");
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
  uint32_t ultimo_reintento_wifi = 0;
  bool servicios_inicializados = (WiFi.status() == WL_CONNECTED);

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      // Ante caída o error de conexión inalámbrica, el sistema obedece
      // inmediatamente al hardware físico
      if (servicios_inicializados || control_interfaz_activo) {
        portENTER_CRITICAL(&timerMux);
        control_interfaz_activo = false;
        modo_adc_potenciometro = true;
        portEXIT_CRITICAL(&timerMux);
        servicios_inicializados = false;
        Serial.println(
            "[Core 0] Conexión Wi-Fi perdida: Conmutando automáticamente a "
            "control por Hardware (Potenciómetro + Pin Pull-Up).");
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

          // Leer estado actual para conservar valores que no vengan
          // especificados en el paquete
          portENTER_CRITICAL(&timerMux);
          bool onda_completa_actual = modo_onda_completa;
          bool usar_adc_actual = modo_adc_potenciometro;
          float angulo_actual = angulo_disparo_deg;
          portEXIT_CRITICAL(&timerMux);

          bool comando_valido = false;
          bool error_rango = false;
          float alfa_recibido = angulo_actual;

          // 1. Evaluar selector de onda si está presente en el mensaje
          // ("ONDA:COMPLETA" o "ONDA:MEDIA")
          if (mensaje.indexOf("ONDA:COMPLETA") >= 0) {
            onda_completa_actual = true;
            comando_valido = true;
          } else if (mensaje.indexOf("ONDA:MEDIA") >= 0) {
            onda_completa_actual = false;
            comando_valido = true;
          }

          // 2. Evaluar comando de ángulo o modo ADC ("ALFA:ADC", "MODO:ADC" o
          // "ALFA:xx.x")
          if (mensaje.indexOf("ALFA:ADC") >= 0 ||
              mensaje.indexOf("MODO:ADC") >= 0) {
            usar_adc_actual = true;
            comando_valido = true;
          } else if (mensaje.startsWith("ALFA:")) {
            int idx_fin = mensaje.indexOf(';');
            String sub_alfa = (idx_fin > 5) ? mensaje.substring(5, idx_fin)
                                            : mensaje.substring(5);
            alfa_recibido = sub_alfa.toFloat();

            if (alfa_recibido >= 0.0f && alfa_recibido <= 180.0f) {
              usar_adc_actual = false;
              angulo_actual = alfa_recibido;
              comando_valido = true;
            } else {
              error_rango = true;
            }
          }

          // 3. Aplicar cambios y enviar confirmación (ACK/NACK) a la interfaz
          // gráfica
          if (error_rango) {
            Serial.printf(
                "[Core 0][UDP] Ángulo fuera de rango (0-180°): %.1f°\n",
                alfa_recibido);
            udp.beginPacket(udp.remoteIP(), udp.remotePort());
            udp.printf("NACK:OUT_OF_RANGE:%.1f\n", alfa_recibido);
            udp.endPacket();
          } else if (comando_valido) {
            actualizar_parametros_disparo(angulo_actual, usar_adc_actual,
                                          onda_completa_actual, true);
            const char *str_onda = onda_completa_actual ? "COMPLETA" : "MEDIA";

            if (usar_adc_actual) {
              Serial.printf("[Core 0][UDP] Modo ADC activo | Onda: %s\n",
                            str_onda);
              udp.beginPacket(udp.remoteIP(), udp.remotePort());
              udp.printf("ACK:ALFA:ADC:%.1f;ONDA:%s\n", angulo_disparo_deg,
                         str_onda);
              udp.endPacket();
            } else {
              Serial.printf(
                  "[Core 0][UDP] Comando aplicado: ALFA = %.1f° | Onda: %s\n",
                  angulo_actual, str_onda);
              udp.beginPacket(udp.remoteIP(), udp.remotePort());
              udp.printf("ACK:ALFA:%.1f;ONDA:%s\n", angulo_actual, str_onda);
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
  Serial.println("\n--- InverRect-Monitor: Inicializando Firmware ESP32 "
                 "(Circuito de Disparo Único) ---");

  // 1. Configuración de Pines GPIO
  pinMode(PIN_DISPARO, OUTPUT);
  digitalWrite(PIN_DISPARO, LOW);
  pinMode(PIN_ZERO_CROSS, INPUT);
  pinMode(PIN_SELECTOR_ONDA,
          INPUT_PULLUP); // Pull-up para selección por hardware (HIGH = Onda
                         // Completa, LOW = Media Onda)

  // Configuración del ADC1 de 12 bits (0-4095) para Potenciómetro
  analogReadResolution(12);

  // 2. Evaluar estado inicial del hardware local antes de establecer conexión
  bool onda_hw_inicial = (digitalRead(PIN_SELECTOR_ONDA) == HIGH);
  int adc_inicial = analogRead(PIN_POTENTIOMETER);
  float angulo_inicial = (float)map(adc_inicial, 0, 4095, 5, 175);
  actualizar_parametros_disparo(angulo_inicial, true, onda_hw_inicial, false);

  // 3. Adjuntar interrupción ZCD en ambos flancos (CHANGE) para detectar todos
  // los cruces por cero (cada 10 ms)
  attachInterrupt(digitalPinToInterrupt(PIN_ZERO_CROSS), isrCrucePorCero,
                  CHANGE);

  // 4. Crear tarea de red FreeRTOS asignada al Core 0
  xTaskCreatePinnedToCore(TaskComunicaciones, "TaskComunicaciones", 10000, NULL,
                          1, &ManejadorOTA,
                          0 // Core 0
  );

  Serial.println(
      "[Core 1] Interrupción ZCD (todos los cruces) en GPIO 4 activa.");
  Serial.println("--- Sistema listo (Disparo único en GPIO 16 / Potenciómetro "
                 "GPIO 36 / Selector Onda HW GPIO 17) ---");
}

// ==========================================
// 8. BUCLE PRINCIPAL DE POTENCIA (Arduino Standard Loop)
// ==========================================
void loop() {
  // 1. Evaluación de controles fuera de la ventana crítica de espera del
  // disparo
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

    // Si NO hay conexión activa con la interfaz, evaluar directamente el
    // hardware (Pin Pull-Up y Potenciómetro)
    if (!interfaz_activa) {
      onda_actual = (digitalRead(PIN_SELECTOR_ONDA) ==
                     HIGH); // HIGH = Onda Completa, LOW = Media Onda
      int lecturaADC = analogRead(PIN_POTENTIOMETER);
      angulo_actual = (float)map(lecturaADC, 0, 4095, 5, 175);
      actualizar_parametros_disparo(angulo_actual, true, onda_actual, false);
    }
    // Si hay conexión con la interfaz, obedecer el modo de onda de la interfaz
    // y leer ADC solo si se seleccionó modo ADC
    else if (usar_adc) {
      int lecturaADC = analogRead(PIN_POTENTIOMETER);
      angulo_actual = (float)map(lecturaADC, 0, 4095, 5, 175);
      actualizar_parametros_disparo(angulo_actual, true, onda_actual, true);
    } else {
      // Actualizar el retardo en microsegundos por si varió ligeramente el
      // periodo medido de la red
      portENTER_CRITICAL(&timerMux);
      retardo_disparo_us =
          calcular_retardo_us(angulo_disparo_deg, periodoSemiciclo);
      portEXIT_CRITICAL(&timerMux);
    }
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
    inputStr.toUpperCase();

    if (inputStr.equals("ADC")) {
      actualizar_parametros_disparo(angulo_disparo_deg, true,
                                    modo_onda_completa, true);
      Serial.println("[Serial] Modo ADC Potenciómetro ACTIVADO.");
    } else if (inputStr.equals("COMPLETA")) {
      actualizar_parametros_disparo(angulo_disparo_deg, modo_adc_potenciometro,
                                    true, true);
      Serial.println(
          "[Serial] Salida configurada en ONDA COMPLETA (todos los cruces).");
    } else if (inputStr.equals("MEDIA")) {
      actualizar_parametros_disparo(angulo_disparo_deg, modo_adc_potenciometro,
                                    false, true);
      Serial.println(
          "[Serial] Salida configurada en MEDIA ONDA (disparo intercalado).");
    } else if (inputStr.equals("HW")) {
      actualizar_parametros_disparo(angulo_disparo_deg, true,
                                    modo_onda_completa, false);
      Serial.println("[Serial] Control cedido íntegramente al HARDWARE local.");
    } else {
      float nuevo_angulo = inputStr.toFloat();
      if (nuevo_angulo >= 0.0f && nuevo_angulo <= 180.0f) {
        actualizar_parametros_disparo(nuevo_angulo, false, modo_onda_completa,
                                      true);
        Serial.printf("[Serial] Nuevo ángulo fijado: %.1f°\n", nuevo_angulo);
      }
    }
  }
}
