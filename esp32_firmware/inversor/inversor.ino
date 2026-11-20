#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "inverter_mcpwm.h"
#include "acquisition.h"
#include "comms.h"

/**
 * ============================================================================
 * Proyecto: InverRect-Monitor - Control de Inversor Monofásico con ESP32
 * Archivo:  esp32_firmware/inversor/inversor.ino (Sketch compatible con Arduino IDE)
 * Carpeta:  esp32_firmware/inversor/
 * Hardware: PCB/Inversor/Inversor.kicad_pcb (ESP32-DEVKITC-32E, 2x IR2110, 4x IRF840A)
 *
 * Arquitectura Dual-Core (FreeRTOS):
 *  - Core 1 (Alta Prioridad):
 *      * Generación 100% por hardware MCPWM a 50 Hz (Phase-Shift Control con
 *        Dead-Time por hardware RED/FED de 50 us anti shoot-through).
 *      * Lectura con antirrebote del jumper físico WS (GPIO 27).
 *      * Adquisición determinística ADC1 de 64 muestras/ciclo (ADC_VP, ADC_VN, Viout)
 *        sin cálculos estadísticos en el ESP32 (se envían crudas/escaladas a Python).
 *      * Consola Serial de pruebas de banco a 115200 baudios.
 *  - Core 0 (Conectividad):
 *      * Wi-Fi STA + ArduinoOTA (con parada segura del puente H en onStart).
 *      * Servidor UDP SCADA en puerto 8888 compatible con pc_software/interfaz.py.
 * ============================================================================
 */

// ============================================================================
// INSTANCIAS GLOBALES COMPARTIDAS ENTRE CORE 0 Y CORE 1
// ============================================================================
portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;

volatile InverterState g_inverterState = {
    false,          // running
    false,          // estop_latched
    false,          // control_interfaz_activo (arranca obedeciendo al jumper WS)
    false,          // jumper_ws_closed
    WAVE_SQUARE,    // wave_mode
    ALPHA_MIN_DEG   // alpha_deg
};

// Estado de antirrebote por software del jumper físico WS (GPIO 27)
static bool s_wsRawPrev = true;
static bool s_wsStableState = true; // true = HIGH (abierto = Cuadrada), false = LOW (cerrado = Cuasicuadrada)
static uint32_t s_wsLastEdgeMs = 0;

// ============================================================================
// LECTURA CON ANTIRREBOTE DEL JUMPER FÍSICO WS (GPIO 27)
// ============================================================================
static void pollWaveSelectorJumper() {
    bool lectura = (digitalRead(PIN_WAVE_SELECTOR) == HIGH);
    uint32_t ahora_ms = millis();

    if (lectura != s_wsRawPrev) {
        s_wsRawPrev = lectura;
        s_wsLastEdgeMs = ahora_ms;
    }

    if ((ahora_ms - s_wsLastEdgeMs) >= WS_DEBOUNCE_MS) {
        bool cambio_jumper = (lectura != s_wsStableState);
        s_wsStableState = lectura;

        bool ws_closed = !s_wsStableState; // Activo en bajo (jumper a GND)
        bool gui_activa;
        bool running;
        bool estop;
        InverterWaveMode modo_actual;
        float alpha_actual;

        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.jumper_ws_closed = ws_closed;
        gui_activa   = g_inverterState.control_interfaz_activo;
        running      = g_inverterState.running;
        estop        = g_inverterState.estop_latched;
        modo_actual  = g_inverterState.wave_mode;
        alpha_actual = g_inverterState.alpha_deg;
        portEXIT_CRITICAL(&g_stateMux);

        // En modo local (sin override de la GUI), obedecer siempre el jumper físico WS
        if (!gui_activa && !estop && running) {
            InverterWaveMode modo_objetivo = ws_closed ? WAVE_QUASI_SQUARE : WAVE_SQUARE;
            float alpha_objetivo = ws_closed ? ALPHA_QUASI_DEFAULT_DEG : ALPHA_MIN_DEG;

            if (cambio_jumper || modo_objetivo != modo_actual || fabsf(alpha_objetivo - alpha_actual) > 0.05f) {
                inverterSetAlphaDeg(alpha_objetivo);
                portENTER_CRITICAL(&g_stateMux);
                g_inverterState.wave_mode = modo_objetivo;
                g_inverterState.alpha_deg = alpha_objetivo;
                portEXIT_CRITICAL(&g_stateMux);

                Serial.printf("[Core 1][WS] Jumper %s -> Onda %s (alpha = %.1f°)\n",
                              ws_closed ? "CERRADO (GND)" : "ABIERTO (3.3V)",
                              ws_closed ? "CUASICUADRADA" : "CUADRADA",
                              alpha_objetivo);
            }
        }
    }
}

// ============================================================================
// CONSOLA SERIAL DE PRUEBAS DE BANCO (115200 BPS)
// ============================================================================
static void pollSerialConsole() {
    if (Serial.available() <= 0) {
        return;
    }

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    cmd.toUpperCase();
    if (cmd.length() == 0) {
        return;
    }

    if (cmd.equals("STOP") || cmd.equals("ESTOP")) {
        inverterEmergencyStop();
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.running = false;
        g_inverterState.estop_latched = true;
        portEXIT_CRITICAL(&g_stateMux);
        Serial.println("[Serial] PARADA DE EMERGENCIA: Vin1..Vin4 forzados en LOW.");
    } else if (cmd.equals("START")) {
        float alfa;
        InverterWaveMode modo;
        portENTER_CRITICAL(&g_stateMux);
        alfa = g_inverterState.alpha_deg;
        modo = g_inverterState.wave_mode;
        portEXIT_CRITICAL(&g_stateMux);
        if (modo == WAVE_SQUARE) alfa = ALPHA_MIN_DEG;

        if (inverterStart(alfa)) {
            portENTER_CRITICAL(&g_stateMux);
            g_inverterState.running = true;
            g_inverterState.estop_latched = false;
            g_inverterState.alpha_deg = alfa;
            portEXIT_CRITICAL(&g_stateMux);
            Serial.printf("[Serial] Inversor ARRANCADO: alpha = %.1f° (%s)\n",
                          alfa, (modo == WAVE_QUASI_SQUARE) ? "CUASICUADRADA" : "CUADRADA");
        } else {
            Serial.println("[Serial] ERROR al arrancar MCPWM.");
        }
    } else if (cmd.equals("HW")) {
        bool ws_closed = (digitalRead(PIN_WAVE_SELECTOR) == LOW);
        InverterWaveMode modo_hw = ws_closed ? WAVE_QUASI_SQUARE : WAVE_SQUARE;
        float alfa_hw = ws_closed ? ALPHA_QUASI_DEFAULT_DEG : ALPHA_MIN_DEG;
        bool running;
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.control_interfaz_activo = false;
        g_inverterState.jumper_ws_closed = ws_closed;
        g_inverterState.wave_mode = modo_hw;
        g_inverterState.alpha_deg = alfa_hw;
        running = g_inverterState.running;
        portEXIT_CRITICAL(&g_stateMux);
        if (running) inverterSetAlphaDeg(alfa_hw);
        Serial.printf("[Serial] Control devuelto al Jumper WS (%s -> %.1f°).\n",
                      ws_closed ? "CERRADO" : "ABIERTO", alfa_hw);
    } else if (cmd.equals("CUADRADA")) {
        if (commsApplyGuiCommand(ALPHA_MIN_DEG, WAVE_SQUARE, true)) {
            Serial.println("[Serial] Onda CUADRADA fijada (alpha = 0.0°).");
        } else {
            Serial.println("[Serial] Rechazado (verifique si STOP esta activo; envie START).");
        }
    } else if (cmd.equals("CUASI")) {
        if (commsApplyGuiCommand(ALPHA_QUASI_DEFAULT_DEG, WAVE_QUASI_SQUARE, true)) {
            Serial.printf("[Serial] Onda CUASICUADRADA fijada (alpha = %.1f°).\n", ALPHA_QUASI_DEFAULT_DEG);
        } else {
            Serial.println("[Serial] Rechazado (verifique si STOP esta activo; envie START).");
        }
    } else if (cmd.equals("STATUS")) {
        InverterState st;
        portENTER_CRITICAL(&g_stateMux);
        st.running = g_inverterState.running;
        st.estop_latched = g_inverterState.estop_latched;
        st.control_interfaz_activo = g_inverterState.control_interfaz_activo;
        st.jumper_ws_closed = g_inverterState.jumper_ws_closed;
        st.wave_mode = g_inverterState.wave_mode;
        st.alpha_deg = g_inverterState.alpha_deg;
        portEXIT_CRITICAL(&g_stateMux);

        float v_buf[NUM_MUESTRAS_CICLO];
        float i_buf[NUM_MUESTRAS_CICLO];
        acquisitionGetLatestCycle(v_buf, i_buf);
        float vmin = v_buf[0], vmax = v_buf[0], imin = i_buf[0], imax = i_buf[0];
        for (int k = 1; k < NUM_MUESTRAS_CICLO; k++) {
            if (v_buf[k] < vmin) vmin = v_buf[k];
            if (v_buf[k] > vmax) vmax = v_buf[k];
            if (i_buf[k] < imin) imin = i_buf[k];
            if (i_buf[k] > imax) imax = i_buf[k];
        }
        Serial.printf("[STATUS] Run=%d | EStop=%d | Fuente=%s | WS=%s | Onda=%s | Alpha=%.1f° | DeadTime=%u us\n",
                      st.running ? 1 : 0,
                      st.estop_latched ? 1 : 0,
                      st.control_interfaz_activo ? "GUI/Serial" : "Jumper WS",
                      st.jumper_ws_closed ? "CERRADO" : "ABIERTO",
                      (st.wave_mode == WAVE_QUASI_SQUARE) ? "CUASICUADRADA" : "CUADRADA",
                      st.alpha_deg,
                      (unsigned)DEAD_TIME_US);
        Serial.printf("         OffsetACS=%.4f V | Ultimo ciclo V=[%.1f, %.1f] V | I=[%.3f, %.3f] A\n",
                      acquisitionGetAcsOffsetPinV(), vmin, vmax, imin, imax);
    } else {
        char *endptr = nullptr;
        float nuevo_alfa = strtof(cmd.c_str(), &endptr);
        if (endptr != cmd.c_str() && nuevo_alfa >= ALPHA_MIN_DEG && nuevo_alfa <= ALPHA_MAX_DEG) {
            InverterWaveMode modo = (nuevo_alfa > 0.05f) ? WAVE_QUASI_SQUARE : WAVE_SQUARE;
            if (commsApplyGuiCommand(nuevo_alfa, modo, true)) {
                Serial.printf("[Serial] Angulo alpha fijado: %.1f° (%s)\n",
                              nuevo_alfa, (modo == WAVE_QUASI_SQUARE) ? "CUASICUADRADA" : "CUADRADA");
            } else {
                Serial.println("[Serial] Rechazado (STOP activo; envie START primero).");
            }
        } else {
            Serial.println("[Serial] Comandos: CUADRADA | CUASI | <0..60> | HW | STOP | START | STATUS");
        }
    }
}

// ============================================================================
// SETUP: SECUENCIA DE ARRANQUE SEGURO (SAFE-BOOT)
// ============================================================================
void setup() {
    // 1. PRIORIDAD ABSOLUTA DE SEGURIDAD:
    //    Forzar Vin1..Vin4 en LOW antes de inicializar Serial, ADC, MCPWM o Wi-Fi.
    inverterSafeBootPinsLow();

    Serial.begin(115200);
    delay(300);
    Serial.println("\n======================================================================");
    Serial.println("  InverRect-Monitor - Firmware Inversor Monofasico 50 Hz (MCPWM)");
    Serial.printf("  Dead-Time Hardware: %u us | Muestras/Ciclo: %d (3200 Hz)\n",
                  (unsigned)DEAD_TIME_US, NUM_MUESTRAS_CICLO);
    Serial.println("======================================================================");

    // 2. Configurar selector físico de forma de onda (Jumper WS en GPIO 27)
    pinMode(PIN_WAVE_SELECTOR, INPUT_PULLUP);
    delay(5);
    s_wsRawPrev = (digitalRead(PIN_WAVE_SELECTOR) == HIGH);
    s_wsStableState = s_wsRawPrev;
    s_wsLastEdgeMs = millis();

    bool ws_closed_inicial = !s_wsStableState;
    InverterWaveMode modo_inicial = ws_closed_inicial ? WAVE_QUASI_SQUARE : WAVE_SQUARE;
    float alpha_inicial = ws_closed_inicial ? ALPHA_QUASI_DEFAULT_DEG : ALPHA_MIN_DEG;

    // 3. Configurar pines ADC1 y calibrar el offset del ACS712 con el puente H APAGADO (0 A)
    acquisitionInitPins();
    acquisitionCalibrateZeroCurrent();

    // 4. Arrancar puente H: pre-carga segura de bootstrap (5 ms solo Low-Side) + MCPWM 50 Hz
    if (inverterStart(alpha_inicial)) {
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.running = true;
        g_inverterState.estop_latched = false;
        g_inverterState.control_interfaz_activo = false;
        g_inverterState.jumper_ws_closed = ws_closed_inicial;
        g_inverterState.wave_mode = modo_inicial;
        g_inverterState.alpha_deg = alpha_inicial;
        portEXIT_CRITICAL(&g_stateMux);

        Serial.printf("[Core 1] MCPWM 50 Hz ACTIVO | Jumper WS=%s -> Onda %s (alpha=%.1f°)\n",
                      ws_closed_inicial ? "CERRADO" : "ABIERTO",
                      ws_closed_inicial ? "CUASICUADRADA" : "CUADRADA",
                      alpha_inicial);
    } else {
        inverterEmergencyStop();
        portENTER_CRITICAL(&g_stateMux);
        g_inverterState.running = false;
        g_inverterState.estop_latched = true;
        portEXIT_CRITICAL(&g_stateMux);
        Serial.println("[Core 1] ERROR CRITICO: Fallo al iniciar MCPWM. Puente H detenido en LOW.");
    }

    // 5. Iniciar tarea de comunicaciones (Wi-Fi + UDP puerto 8888 + ArduinoOTA) en Core 0
    commsStartTaskOnCore0();

    Serial.println("[Core 1] Pines activos -> Pierna P: Vin1=IO33, Vin4=IO32 | Pierna N: Vin2=IO26, Vin3=IO25");
    Serial.println("[Core 1] ADC1 -> ADC_VP=IO36, ADC_VN=IO35, Viout=IO39 | WS=IO27");
    Serial.println("[Serial] Comandos disponibles: CUADRADA | CUASI | <0..60> | HW | STOP | START | STATUS");
}

// ============================================================================
// LOOP PRINCIPAL (CORE 1): ADQUISICIÓN ADC1 + SUPERVISIÓN JUMPER WS + SERIAL
// Nota: Los pulsos de 50 Hz y el dead-time de 50 us se generan 100% por hardware MCPWM.
// ============================================================================
void loop() {
    // 1. Muestreo determinístico de V(t) e I(t) (64 muestras/ciclo sincronizadas al MCPWM)
    acquisitionPoll();

    // 2. Supervisión con antirrebote (50 ms) del jumper físico WS (GPIO 27)
    pollWaveSelectorJumper();

    // 3. Consola Serial para pruebas locales y verificación en osciloscopio
    pollSerialConsole();
}
