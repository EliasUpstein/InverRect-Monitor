#include "comms.h"
#include "config.h"
#include "inverter_mcpwm.h"
#include "acquisition.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static WiFiUDP s_udp;
static TaskHandle_t s_taskCommsHandle = nullptr;

// ============================================================================
// UTILIDADES DE ESTADO Y FORMATO DE TRAMAS
// ============================================================================
static const char *waveModeToStr(InverterWaveMode mode, bool running) {
    if (!running) {
        return "STOP";
    }
    return (mode == WAVE_QUASI_SQUARE) ? "CUASICUADRADA" : "CUADRADA";
}

bool commsApplyGuiCommand(float alpha_deg, InverterWaveMode mode, bool from_gui) {
    // En modo Onda Cuadrada el ángulo efectivo es siempre 0.0°
    float alpha_efectivo = (mode == WAVE_SQUARE) ? ALPHA_MIN_DEG : alpha_deg;
    if (alpha_efectivo < ALPHA_MIN_DEG || alpha_efectivo > ALPHA_MAX_DEG) {
        return false;
    }

    bool estop_activo = false;
    portENTER_CRITICAL(&g_stateMux);
    estop_activo = g_inverterState.estop_latched;
    portEXIT_CRITICAL(&g_stateMux);

    if (estop_activo) {
        return false;
    }

    inverterSetAlphaDeg(alpha_efectivo);

    portENTER_CRITICAL(&g_stateMux);
    g_inverterState.wave_mode = mode;
    g_inverterState.alpha_deg = alpha_efectivo;
    g_inverterState.control_interfaz_activo = from_gui;
    portEXIT_CRITICAL(&g_stateMux);

    return true;
}

static void configureOtaCallbacks() {
    // Registrar siempre los callbacks de seguridad OTA (incluso si Wi-Fi conecta más tarde)
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        // CRITICO: Apagar inmediatamente los 4 MOSFETs (Vin1..Vin4 = LOW) antes de flashear
        inverterEmergencyStop();
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.running = false;
        g_inverterState.estop_latched = true;
        portEXIT_CRITICAL(&g_stateMux);
        Serial.println("[OTA] Inicio de flasheo inalambrico! Puente H apagado (Vin1..Vin4 = LOW).");
    });

    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] Actualizacion completada con exito. Reiniciando...");
    });

    ArduinoOTA.onError([](ota_error_t error) {
        inverterEmergencyStop();
        Serial.printf("[OTA] Error[%u] al actualizar. Puente H mantenido en estado seguro LOW.\n", error);
    });
}

static void setupWifiAndOta() {
    configureOtaCallbacks();

    Serial.println("[Core 0] Iniciando conexion Wi-Fi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int intentos = 0;
    const int max_intentos = 30; // Timeout 15 s
    while (WiFi.status() != WL_CONNECTED && intentos < max_intentos) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
        intentos++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[Core 0] Wi-Fi conectado con exito!");
        Serial.print("[Core 0] Direccion IP asignada: ");
        Serial.println(WiFi.localIP());

        ArduinoOTA.begin();
        s_udp.begin(UDP_PORT);
        Serial.printf("[Core 0] Servidor UDP SCADA activo en puerto %u (OTA host: %s).\n",
                      UDP_PORT, OTA_HOSTNAME);
    } else {
        Serial.println("\n[Core 0] Aviso: Sin conexion Wi-Fi (Timeout 15 s). Obedeciendo jumper fisico WS (GPIO 27).");
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.control_interfaz_activo = false;
        portEXIT_CRITICAL(&g_stateMux);
    }
}

// ============================================================================
// PROCESAMIENTO DE DATAGRAMAS UDP (PROTOCOLO SCADA)
// ============================================================================
static void handleUdpPacket(const String &mensaje, char *txBuffer, size_t txBufferSize) {
    static float copia_v[NUM_MUESTRAS_CICLO];
    static float copia_i[NUM_MUESTRAS_CICLO];

    // 1. Solicitud de vectores de muestras V e I (el ESP32 NO hace cálculos estadísticos)
    if (mensaje.startsWith("GET_ADC") || mensaje.startsWith("LEER_ADC")) {
        float alfa_snap;
        InverterWaveMode mode_snap;
        bool running_snap;

        portENTER_CRITICAL(&g_stateMux);
        alfa_snap    = g_inverterState.alpha_deg;
        mode_snap    = g_inverterState.wave_mode;
        running_snap = g_inverterState.running;
        portEXIT_CRITICAL(&g_stateMux);

        acquisitionGetLatestCycle(copia_v, copia_i);

        const char *str_onda = waveModeToStr(mode_snap, running_snap);
        int pos = snprintf(
            txBuffer,
            txBufferSize,
            "ADC_DATA:ALFA:%.1f;ONDA:%s;V:",
            alfa_snap,
            str_onda
        );

        for (int k = 0; k < NUM_MUESTRAS_CICLO && pos < (int)txBufferSize - 24; k++) {
            pos += snprintf(
                txBuffer + pos,
                txBufferSize - pos,
                (k < NUM_MUESTRAS_CICLO - 1) ? "%.2f," : "%.2f",
                copia_v[k]
            );
        }

        pos += snprintf(txBuffer + pos, txBufferSize - pos, ";I:");

        for (int k = 0; k < NUM_MUESTRAS_CICLO && pos < (int)txBufferSize - 16; k++) {
            pos += snprintf(
                txBuffer + pos,
                txBufferSize - pos,
                (k < NUM_MUESTRAS_CICLO - 1) ? "%.3f," : "%.3f\n",
                copia_i[k]
            );
        }

        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.write((const uint8_t *)txBuffer, pos);
        s_udp.endPacket();
        return;
    }

    // 2. Parada de Emergencia (Fail-Safe inmediato por hardware)
    if (mensaje.equals("STOP") || mensaje.equals("ESTOP") || mensaje.equals("EMERGENCIA")) {
        inverterEmergencyStop();
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.running = false;
        g_inverterState.estop_latched = true;
        portEXIT_CRITICAL(&g_stateMux);

        Serial.println("[Core 0][UDP] PARADA DE EMERGENCIA ACTIVADA -> Vin1..Vin4 en LOW.");
        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.printf("ACK:STOP\n");
        s_udp.endPacket();
        return;
    }

    // 3. Reanudar tras Parada de Emergencia (pre-carga segura de bootstrap + MCPWM)
    if (mensaje.equals("START") || mensaje.equals("RESUME")) {
        float alfa_arranque;
        InverterWaveMode mode_arranque;

        portENTER_CRITICAL(&g_stateMux);
        alfa_arranque = g_inverterState.alpha_deg;
        mode_arranque = g_inverterState.wave_mode;
        portEXIT_CRITICAL(&g_stateMux);

        if (mode_arranque == WAVE_SQUARE) {
            alfa_arranque = ALPHA_MIN_DEG;
        }

        if (inverterStart(alfa_arranque)) {
            portENTER_CRITICAL(&g_stateMux);
            g_inverterState.running = true;
            g_inverterState.estop_latched = false;
            g_inverterState.alpha_deg = alfa_arranque;
            portEXIT_CRITICAL(&g_stateMux);

            const char *str_onda = waveModeToStr(mode_arranque, true);
            Serial.printf("[Core 0][UDP] Inversor REANUDADO: ALFA=%.1f° | ONDA=%s\n", alfa_arranque, str_onda);
            s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
            s_udp.printf("ACK:START;ALFA:%.1f;ONDA:%s\n", alfa_arranque, str_onda);
            s_udp.endPacket();
        } else {
            s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
            s_udp.printf("NACK:START_FAILED\n");
            s_udp.endPacket();
        }
        return;
    }

    // 4. Retorno explícito a control local por Jumper físico WS (GPIO 27)
    if (mensaje.equals("HW") || mensaje.equals("MODO:HW") || mensaje.equals("ALFA:HW")) {
        bool ws_closed = (digitalRead(PIN_WAVE_SELECTOR) == LOW);
        InverterWaveMode mode_hw = ws_closed ? WAVE_QUASI_SQUARE : WAVE_SQUARE;
        float alfa_hw = ws_closed ? ALPHA_QUASI_DEFAULT_DEG : ALPHA_MIN_DEG;

        bool running_snap;
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.control_interfaz_activo = false;
        g_inverterState.jumper_ws_closed = ws_closed;
        g_inverterState.wave_mode = mode_hw;
        g_inverterState.alpha_deg = alfa_hw;
        running_snap = g_inverterState.running;
        portEXIT_CRITICAL(&g_stateMux);

        if (running_snap) {
            inverterSetAlphaDeg(alfa_hw);
        }

        const char *str_onda = waveModeToStr(mode_hw, running_snap);
        Serial.printf("[Core 0][UDP] Control devuelto al Jumper WS (%s) -> ALFA=%.1f° | ONDA=%s\n",
                      ws_closed ? "CERRADO" : "ABIERTO", alfa_hw, str_onda);
        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.printf("ACK:MODO:HW;ALFA:%.1f;ONDA:%s\n", alfa_hw, str_onda);
        s_udp.endPacket();
        return;
    }

    // 5. Consulta de diagnóstico rápido
    if (mensaje.equals("STATUS")) {
        InverterState st;
        portENTER_CRITICAL(&g_stateMux);
        st.running = g_inverterState.running;
        st.estop_latched = g_inverterState.estop_latched;
        st.control_interfaz_activo = g_inverterState.control_interfaz_activo;
        st.jumper_ws_closed = g_inverterState.jumper_ws_closed;
        st.wave_mode = g_inverterState.wave_mode;
        st.alpha_deg = g_inverterState.alpha_deg;
        portEXIT_CRITICAL(&g_stateMux);

        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.printf("STATUS:RUN=%d;ESTOP=%d;SRC=%s;WS=%d;ALFA=%.1f;ONDA=%s;VZCD=%d;IOFF=%.3f\n",
                     st.running ? 1 : 0,
                     st.estop_latched ? 1 : 0,
                     st.control_interfaz_activo ? "GUI" : "HW",
                     st.jumper_ws_closed ? 1 : 0,
                     st.alpha_deg,
                     waveModeToStr(st.wave_mode, st.running),
                     acquisitionIsVzcdPositive() ? 1 : 0,
                     acquisitionGetAcsOffsetPinV());
        s_udp.endPacket();
        return;
    }

    // 6. Comandos de configuración de onda y ángulo alpha:
    //    Ejemplos: "ALFA:0.0;ONDA:CUADRADA", "ALFA:30.0;ONDA:CUASICUADRADA", "ALFA:45.0"
    InverterWaveMode mode_actual;
    float alfa_actual;
    bool estop_activo;

    portENTER_CRITICAL(&g_stateMux);
    mode_actual  = g_inverterState.wave_mode;
    alfa_actual  = g_inverterState.alpha_deg;
    estop_activo = g_inverterState.estop_latched;
    portEXIT_CRITICAL(&g_stateMux);

    if (estop_activo) {
        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.printf("NACK:ESTOP_ACTIVE\n");
        s_udp.endPacket();
        return;
    }

    bool comando_valido = false;
    bool error_rango = false;
    bool tiene_alfa = false;
    bool onda_explicita = false;
    bool onda_explicita_cuadrada = false;
    float alfa_recibido = alfa_actual;

    // 6.1 Evaluar modo de onda
    if (mensaje.indexOf("ONDA:CUASICUADRADA") >= 0 || mensaje.indexOf("ONDA:CUASI") >= 0) {
        mode_actual = WAVE_QUASI_SQUARE;
        onda_explicita = true;
        comando_valido = true;
    } else if (mensaje.indexOf("ONDA:CUADRADA") >= 0) {
        mode_actual = WAVE_SQUARE;
        onda_explicita = true;
        onda_explicita_cuadrada = true;
        alfa_recibido = ALPHA_MIN_DEG;
        comando_valido = true;
    }

    // 6.2 Evaluar parámetro ALFA:<deg>
    int idx_alfa = mensaje.indexOf("ALFA:");
    if (idx_alfa >= 0) {
        int inicio = idx_alfa + 5;
        int idx_fin = mensaje.indexOf(';', inicio);
        String sub_alfa = (idx_fin > inicio) ? mensaje.substring(inicio, idx_fin) : mensaje.substring(inicio);
        sub_alfa.trim();

        if (sub_alfa.equals("HW")) {
            // Alias de retorno a jumper físico
            handleUdpPacket("HW", txBuffer, txBufferSize);
            return;
        }

        // Verificar que la cadena sea numérica válida
        char *endptr = nullptr;
        float val = strtof(sub_alfa.c_str(), &endptr);
        if (endptr == sub_alfa.c_str()) {
            s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
            s_udp.printf("NACK:INVALID_ALFA\n");
            s_udp.endPacket();
            return;
        }

        tiene_alfa = true;
        alfa_recibido = val;

        if (onda_explicita_cuadrada) {
            // En onda cuadrada explícita, alpha se fija en 0.0°
            alfa_recibido = ALPHA_MIN_DEG;
            comando_valido = true;
        } else {
            if (alfa_recibido >= ALPHA_MIN_DEG && alfa_recibido <= ALPHA_MAX_DEG) {
                // Si se envió solo ALFA:xx.x sin ONDA:, inferir modo según si alpha > 0
                if (!onda_explicita) {
                    mode_actual = (alfa_recibido > 0.05f) ? WAVE_QUASI_SQUARE : WAVE_SQUARE;
                }
                comando_valido = true;
            } else {
                error_rango = true;
            }
        }
    } else if (comando_valido && mode_actual == WAVE_QUASI_SQUARE && alfa_recibido < 0.1f) {
        // Si pidieron ONDA:CUASICUADRADA sin especificar ALFA y estaba en 0°, usar 30° por defecto
        alfa_recibido = ALPHA_QUASI_DEFAULT_DEG;
    }

    if (error_rango) {
        Serial.printf("[Core 0][UDP] Angulo fuera de rango seguro (%.0f-%.0f°): %.1f°\n",
                      ALPHA_MIN_DEG, ALPHA_MAX_DEG, alfa_recibido);
        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.printf("NACK:OUT_OF_RANGE:%.1f\n", alfa_recibido);
        s_udp.endPacket();
        return;
    }

    if (comando_valido) {
        float alfa_final = (mode_actual == WAVE_SQUARE) ? ALPHA_MIN_DEG : alfa_recibido;
        if (!tiene_alfa && mode_actual == WAVE_QUASI_SQUARE && alfa_final < ALPHA_MIN_DEG) {
            alfa_final = ALPHA_QUASI_DEFAULT_DEG;
        }

        if (commsApplyGuiCommand(alfa_final, mode_actual, true)) {
            const char *str_onda = waveModeToStr(mode_actual, true);
            Serial.printf("[Core 0][UDP] Comando aplicado: ALFA=%.1f° | ONDA=%s\n", alfa_final, str_onda);
            s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
            s_udp.printf("ACK:ALFA:%.1f;ONDA:%s\n", alfa_final, str_onda);
            s_udp.endPacket();
        } else {
            s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
            s_udp.printf("NACK:APPLY_ERROR\n");
            s_udp.endPacket();
        }
    } else {
        s_udp.beginPacket(s_udp.remoteIP(), s_udp.remotePort());
        s_udp.printf("NACK:UNKNOWN_CMD\n");
        s_udp.endPacket();
    }
}

// ============================================================================
// TAREA FREERTOS EN CORE 0 (WI-FI + ARDUINO OTA + SERVIDOR UDP)
// ============================================================================
static void taskComunicaciones(void *pvParameters) {
    (void)pvParameters;
    setupWifiAndOta();

    char packetBuffer[96];
    static char txBuffer[1400]; // Suficiente para 64 muestras de V y 64 de I en 1 datagrama UDP

    uint32_t ultimo_reintento_wifi = 0;
    bool servicios_inicializados = (WiFi.status() == WL_CONNECTED);

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            // Ante caída Wi-Fi, devolver automáticamente el control al jumper físico WS
            if (servicios_inicializados || g_inverterState.control_interfaz_activo) {
                portENTER_CRITICAL(&g_stateMux);
                g_inverterState.control_interfaz_activo = false;
                portEXIT_CRITICAL(&g_stateMux);
                servicios_inicializados = false;
                Serial.println("[Core 0] Wi-Fi perdido: conmutando automaticamente al jumper fisico WS (GPIO 27).");
            }

            uint32_t ahora = millis();
            if (ahora - ultimo_reintento_wifi > 10000UL) {
                ultimo_reintento_wifi = ahora;
                Serial.println("[Core 0] Reintentando conexion Wi-Fi...");
                WiFi.reconnect();
            }
        } else {
            if (!servicios_inicializados) {
                Serial.println("\n[Core 0] Wi-Fi reconectado!");
                Serial.print("[Core 0] Direccion IP asignada: ");
                Serial.println(WiFi.localIP());
                ArduinoOTA.begin();
                s_udp.begin(UDP_PORT);
                servicios_inicializados = true;
            }

            ArduinoOTA.handle();

            int packetSize = s_udp.parsePacket();
            if (packetSize > 0) {
                int len = s_udp.read(packetBuffer, sizeof(packetBuffer) - 1);
                if (len > 0) {
                    packetBuffer[len] = '\0';
                    String mensaje = String(packetBuffer);
                    mensaje.trim();
                    mensaje.toUpperCase();
                    handleUdpPacket(mensaje, txBuffer, sizeof(txBuffer));
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void commsStartTaskOnCore0() {
    xTaskCreatePinnedToCore(
        taskComunicaciones,
        "TaskComunicacionesInv",
        10000,
        nullptr,
        1,
        &s_taskCommsHandle,
        0 // Core 0
    );
}
