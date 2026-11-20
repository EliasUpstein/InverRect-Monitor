#include "inverter_mcpwm.h"
#include "config.h"

#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_idf_version.h>
#include <esp_timer.h>
#include <driver/gpio.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    #define USE_MCPWM_PRELUDE_API 1
    #include <driver/mcpwm_prelude.h>
#else
    #define USE_MCPWM_PRELUDE_API 0
    #include <driver/mcpwm.h>
#endif

// ============================================================================
// VARIABLES INTERNAS DEL CONTROLADOR MCPWM
// ============================================================================
static SemaphoreHandle_t s_mcpwmMutex = nullptr;
static volatile bool s_running = false;
static volatile bool s_estopRequested = false;
static volatile float s_currentAlphaDeg = ALPHA_MIN_DEG;

// Sincronización de fase para adquisición ADC1 (timestamp en us de esp_timer_get_time)
static volatile uint32_t s_lastTezUs = 0;
static volatile bool s_tezFlag = false;

#if USE_MCPWM_PRELUDE_API
// Handles API moderna ESP-IDF 5.x (Arduino-ESP32 Core 3.x)
static mcpwm_timer_handle_t s_timer = nullptr;
static mcpwm_oper_handle_t  s_operP = nullptr;
static mcpwm_oper_handle_t  s_operN = nullptr;
static mcpwm_cmpr_handle_t  s_cmpPA = nullptr;
static mcpwm_cmpr_handle_t  s_cmpPB = nullptr;
static mcpwm_cmpr_handle_t  s_cmpNA = nullptr;
static mcpwm_cmpr_handle_t  s_cmpNB = nullptr;
static mcpwm_gen_handle_t   s_genPHi = nullptr; // Vin1 (GPIO 33) - Q1
static mcpwm_gen_handle_t   s_genPLo = nullptr; // Vin4 (GPIO 32) - Q2
static mcpwm_gen_handle_t   s_genNHi = nullptr; // Vin2 (GPIO 26) - Q3
static mcpwm_gen_handle_t   s_genNLo = nullptr; // Vin3 (GPIO 25) - Q4

/**
 * ISR de hardware en IRAM: se dispara exactamente cuando Timer 0 cruza por 0 (TEZ, cada 20 ms).
 * Solo registra el timestamp de alta precisión para sincronizar el muestreo ADC en Core 1.
 */
static bool IRAM_ATTR onTimerEmptyISR(mcpwm_timer_handle_t timer,
                                      const mcpwm_timer_event_data_t *edata,
                                      void *user_ctx) {
    (void)timer;
    (void)edata;
    (void)user_ctx;
    s_lastTezUs = (uint32_t)esp_timer_get_time();
    s_tezFlag = true;
    return false;
}
#else
// Referencia temporal para API legacy ESP-IDF 4.x (Arduino-ESP32 Core 2.x)
static volatile uint32_t s_legacyRefUs = 0;
#endif

// ============================================================================
// UTILIDADES INTERNAS
// ============================================================================
static inline float clampAlphaDeg(float alpha_deg) {
    if (isnan(alpha_deg) || isinf(alpha_deg)) return ALPHA_MIN_DEG;
    if (alpha_deg < ALPHA_MIN_DEG) return ALPHA_MIN_DEG;
    if (alpha_deg > ALPHA_MAX_DEG) return ALPHA_MAX_DEG;
    return alpha_deg;
}

static inline uint32_t alphaDegToDeltaTicks(float alpha_deg) {
    // Ancho de cada escalón de 0 V = 2 * alpha sobre 360° -> (alpha / 180) * 20000 us
    float clamped = clampAlphaDeg(alpha_deg);
    return (uint32_t)lroundf((clamped / 180.0f) * (float)MCPWM_PERIOD_TICKS);
}

static void ensureMutexCreated() {
    if (s_mcpwmMutex == nullptr) {
        s_mcpwmMutex = xSemaphoreCreateMutex();
    }
}

static void forceAllGatePinsOutputLow() {
    // 1. Apagar primero los High-Side (Q1, Q3)
    pinMode(PIN_VIN1_P_HIN, OUTPUT);
    digitalWrite(PIN_VIN1_P_HIN, LOW);
    pinMode(PIN_VIN2_N_HIN, OUTPUT);
    digitalWrite(PIN_VIN2_N_HIN, LOW);

    // 2. Apagar los Low-Side (Q2, Q4)
    pinMode(PIN_VIN4_P_LIN, OUTPUT);
    digitalWrite(PIN_VIN4_P_LIN, LOW);
    pinMode(PIN_VIN3_N_LIN, OUTPUT);
    digitalWrite(PIN_VIN3_N_LIN, LOW);
}

/**
 * Pre-carga segura de los capacitores de bootstrap (C7 y C8 de 22 uF):
 * Enciende ÚNICAMENTE los MOSFETs inferiores (Q2 y Q4) durante ~5 ms manteniendo
 * los superiores (Q1 y Q3) estrictamente en LOW, luego apaga todo y espera DEAD_TIME_US.
 */
static bool performBootstrapPrecharge() {
    forceAllGatePinsOutputLow();
    delayMicroseconds(DEAD_TIME_US);

    if (s_estopRequested) {
        forceAllGatePinsOutputLow();
        return false;
    }

    // Confirmación explícita: High-Side estrictamente en LOW antes de activar Low-Side
    digitalWrite(PIN_VIN1_P_HIN, LOW);
    digitalWrite(PIN_VIN2_N_HIN, LOW);
    digitalWrite(PIN_VIN4_P_LIN, HIGH);
    digitalWrite(PIN_VIN3_N_LIN, HIGH);

    // Espera de 5 ms dividida en pasos de 1 ms para abortar de inmediato ante un E-Stop
    for (uint32_t ms = 0; ms < BOOTSTRAP_PRECHARGE_MS; ms++) {
        if (s_estopRequested) {
            forceAllGatePinsOutputLow();
            return false;
        }
        delay(1);
    }

    // Apagar Low-Side y respetar el tiempo muerto antes de ceder los pines al MCPWM
    digitalWrite(PIN_VIN4_P_LIN, LOW);
    digitalWrite(PIN_VIN3_N_LIN, LOW);
    delayMicroseconds(DEAD_TIME_US);

    return !s_estopRequested;
}

#if USE_MCPWM_PRELUDE_API
static void teardownMcpwmPreludeLocked() {
    // Paso 1: Forzar salidas High-Side a LOW y deshabilitar inversión AHC de Low-Side
    // (En ESP32 MCPWM, la inversión de Dead-Time ocurre DESPUÉS del Generator Force)
    if (s_genPHi != nullptr) mcpwm_generator_set_force_level(s_genPHi, 0, true);
    if (s_genNHi != nullptr) mcpwm_generator_set_force_level(s_genNHi, 0, true);
    if (s_genPLo != nullptr) mcpwm_generator_set_force_level(s_genPLo, 0, true);
    if (s_genNLo != nullptr) mcpwm_generator_set_force_level(s_genNLo, 0, true);

    mcpwm_dead_time_config_t dt_bypass = {};
    dt_bypass.posedge_delay_ticks = 0;
    dt_bypass.negedge_delay_ticks = 0;
    dt_bypass.flags.invert_output = false;

    if (s_genPLo != nullptr) mcpwm_generator_set_dead_time(s_genPLo, s_genPLo, &dt_bypass);
    if (s_genNLo != nullptr) mcpwm_generator_set_dead_time(s_genNLo, s_genNLo, &dt_bypass);

    // Paso 2: Deshabilitar timer y liberar recursos en orden inverso (hijos antes que padres)
    if (s_timer != nullptr) {
        mcpwm_timer_disable(s_timer);
    }

    if (s_genPHi != nullptr) { mcpwm_del_generator(s_genPHi); s_genPHi = nullptr; }
    if (s_genPLo != nullptr) { mcpwm_del_generator(s_genPLo); s_genPLo = nullptr; }
    if (s_genNHi != nullptr) { mcpwm_del_generator(s_genNHi); s_genNHi = nullptr; }
    if (s_genNLo != nullptr) { mcpwm_del_generator(s_genNLo); s_genNLo = nullptr; }

    if (s_cmpPA != nullptr)  { mcpwm_del_comparator(s_cmpPA); s_cmpPA = nullptr; }
    if (s_cmpPB != nullptr)  { mcpwm_del_comparator(s_cmpPB); s_cmpPB = nullptr; }
    if (s_cmpNA != nullptr)  { mcpwm_del_comparator(s_cmpNA); s_cmpNA = nullptr; }
    if (s_cmpNB != nullptr)  { mcpwm_del_comparator(s_cmpNB); s_cmpNB = nullptr; }

    if (s_operP != nullptr)  { mcpwm_del_operator(s_operP);   s_operP = nullptr; }
    if (s_operN != nullptr)  { mcpwm_del_operator(s_operN);   s_operN = nullptr; }

    if (s_timer != nullptr)  { mcpwm_del_timer(s_timer);      s_timer = nullptr; }

    // Paso 3: Reconectar explícitamente los 4 pines como GPIO OUTPUT en LOW
    forceAllGatePinsOutputLow();
}
#else
static void teardownMcpwmLegacyLocked() {
    mcpwm_deadtime_disable(MCPWM_UNIT_0, MCPWM_TIMER_0);
    mcpwm_deadtime_disable(MCPWM_UNIT_0, MCPWM_TIMER_1);
    mcpwm_set_signal_low(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A);
    mcpwm_set_signal_low(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B);
    mcpwm_set_signal_low(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A);
    mcpwm_set_signal_low(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B);
    mcpwm_stop(MCPWM_UNIT_0, MCPWM_TIMER_0);
    mcpwm_stop(MCPWM_UNIT_0, MCPWM_TIMER_1);
    forceAllGatePinsOutputLow();
}
#endif

// ============================================================================
// API PÚBLICA
// ============================================================================

void inverterSafeBootPinsLow() {
    forceAllGatePinsOutputLow();
}

bool inverterStart(float initial_alpha_deg) {
    ensureMutexCreated();
    xSemaphoreTake(s_mcpwmMutex, portMAX_DELAY);

    // Si ya estaba corriendo, detener limpiamente primero
    if (s_running) {
#if USE_MCPWM_PRELUDE_API
        teardownMcpwmPreludeLocked();
#else
        teardownMcpwmLegacyLocked();
#endif
        s_running = false;
    }

    s_estopRequested = false;
    float alpha = clampAlphaDeg(initial_alpha_deg);
    uint32_t delta = alphaDegToDeltaTicks(alpha);

    // 1. Pre-carga segura de capacitores de bootstrap (C7, C8) antes de arrancar MCPWM
    if (!performBootstrapPrecharge()) {
        xSemaphoreGive(s_mcpwmMutex);
        return false;
    }

#if USE_MCPWM_PRELUDE_API
    // ========================================================================
    // IMPLEMENTACIÓN ESP-IDF 5.x (Arduino-ESP32 Core 3.x) — driver/mcpwm_prelude.h
    // Arquitectura: 1 único Timer (50 Hz, 1 MHz -> 20000 ticks) + 2 Operadores
    // independientes (Pierna P y Pierna N) con Dead-Time AHC por hardware (50 us).
    // ========================================================================
    esp_err_t err = ESP_OK;

    mcpwm_timer_config_t timer_cfg = {};
    timer_cfg.group_id = 0;
    timer_cfg.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    timer_cfg.resolution_hz = MCPWM_RESOLUTION_HZ;
    timer_cfg.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    timer_cfg.period_ticks = MCPWM_PERIOD_TICKS;
    err = mcpwm_new_timer(&timer_cfg, &s_timer);
    if (err != ESP_OK) { teardownMcpwmPreludeLocked(); xSemaphoreGive(s_mcpwmMutex); return false; }

    // Registrar callback de evento TEZ (count == 0) ANTES de habilitar el timer
    mcpwm_timer_event_callbacks_t cbs = {};
    cbs.on_empty = onTimerEmptyISR;
    err = mcpwm_timer_register_event_callbacks(s_timer, &cbs, nullptr);
    if (err != ESP_OK) { teardownMcpwmPreludeLocked(); xSemaphoreGive(s_mcpwmMutex); return false; }

    // Crear 2 operadores en Grupo 0 y conectarlos al MISMO Timer 0 (0 ns de deriva entre piernas)
    mcpwm_operator_config_t oper_cfg = {};
    oper_cfg.group_id = 0;
    if (mcpwm_new_operator(&oper_cfg, &s_operP) != ESP_OK ||
        mcpwm_new_operator(&oper_cfg, &s_operN) != ESP_OK ||
        mcpwm_operator_connect_timer(s_operP, s_timer) != ESP_OK ||
        mcpwm_operator_connect_timer(s_operN, s_timer) != ESP_OK) {
        teardownMcpwmPreludeLocked();
        xSemaphoreGive(s_mcpwmMutex);
        return false;
    }

    // Crear 4 comparadores con actualización atómica por registro shadow en TEZ (count == 0)
    mcpwm_comparator_config_t cmp_cfg = {};
    cmp_cfg.flags.update_cmp_on_tez = true;
    if (mcpwm_new_comparator(s_operP, &cmp_cfg, &s_cmpPA) != ESP_OK ||
        mcpwm_new_comparator(s_operP, &cmp_cfg, &s_cmpPB) != ESP_OK ||
        mcpwm_new_comparator(s_operN, &cmp_cfg, &s_cmpNA) != ESP_OK ||
        mcpwm_new_comparator(s_operN, &cmp_cfg, &s_cmpNB) != ESP_OK) {
        teardownMcpwmPreludeLocked();
        xSemaphoreGive(s_mcpwmMutex);
        return false;
    }

    // Programar comparadores:
    // Pierna P (fija, 50% duty): LOW en 600 us, HIGH en 10600 us
    // Pierna N (desfasada, 50% duty): HIGH en 600 + delta, LOW en 10600 + delta
    mcpwm_comparator_set_compare_value(s_cmpPA, MCPWM_BASE_OFFSET_TICKS);
    mcpwm_comparator_set_compare_value(s_cmpPB, MCPWM_BASE_OFFSET_TICKS + MCPWM_HALF_PERIOD_TICKS);
    mcpwm_comparator_set_compare_value(s_cmpNA, MCPWM_BASE_OFFSET_TICKS + delta);
    mcpwm_comparator_set_compare_value(s_cmpNB, MCPWM_BASE_OFFSET_TICKS + MCPWM_HALF_PERIOD_TICKS + delta);

    // Crear los 4 generadores asociados a los pines exactos del PCB
    mcpwm_generator_config_t gen_cfg = {};
    gen_cfg.gen_gpio_num = PIN_VIN1_P_HIN;
    err = mcpwm_new_generator(s_operP, &gen_cfg, &s_genPHi);
    if (err != ESP_OK) { teardownMcpwmPreludeLocked(); xSemaphoreGive(s_mcpwmMutex); return false; }

    gen_cfg.gen_gpio_num = PIN_VIN4_P_LIN;
    err = mcpwm_new_generator(s_operP, &gen_cfg, &s_genPLo);
    if (err != ESP_OK) { teardownMcpwmPreludeLocked(); xSemaphoreGive(s_mcpwmMutex); return false; }

    gen_cfg.gen_gpio_num = PIN_VIN2_N_HIN;
    err = mcpwm_new_generator(s_operN, &gen_cfg, &s_genNHi);
    if (err != ESP_OK) { teardownMcpwmPreludeLocked(); xSemaphoreGive(s_mcpwmMutex); return false; }

    gen_cfg.gen_gpio_num = PIN_VIN3_N_LIN;
    err = mcpwm_new_generator(s_operN, &gen_cfg, &s_genNLo);
    if (err != ESP_OK) { teardownMcpwmPreludeLocked(); xSemaphoreGive(s_mcpwmMutex); return false; }

    // Forzar inicialmente genPHi y genNHi en LOW ANTES de activar AHC
    // (Al activar AHC: High-Side = LOW, Low-Side = HIGH -> V_out = 0V seguro)
    mcpwm_generator_set_force_level(s_genPHi, 0, true);
    mcpwm_generator_set_force_level(s_genNHi, 0, true);

    // Configurar acciones de comparación (solo sobre genHi; genLo se deriva en hardware vía Dead-Time AHC)
    mcpwm_gen_compare_event_action_t act = {};
    act.direction = MCPWM_TIMER_DIRECTION_UP;

    // Pierna P: LOW en CMPA (600 us), HIGH en CMPB (10600 us)
    act.comparator = s_cmpPA;
    act.action = MCPWM_GEN_ACTION_LOW;
    mcpwm_generator_set_action_on_compare_event(s_genPHi, act);

    act.comparator = s_cmpPB;
    act.action = MCPWM_GEN_ACTION_HIGH;
    mcpwm_generator_set_action_on_compare_event(s_genPHi, act);

    // Pierna N: HIGH en CMPA (600 + delta), LOW en CMPB (10600 + delta)
    act.comparator = s_cmpNA;
    act.action = MCPWM_GEN_ACTION_HIGH;
    mcpwm_generator_set_action_on_compare_event(s_genNHi, act);

    act.comparator = s_cmpNB;
    act.action = MCPWM_GEN_ACTION_LOW;
    mcpwm_generator_set_action_on_compare_event(s_genNHi, act);

    // Configurar Dead-Time por Hardware Active High Complementary (AHC):
    // - High-Side (genHi): Rising Edge Delay (RED) = DEAD_TIME_US (50 us), sin retardo en flanco bajada.
    // - Low-Side  (genLo): Alimentado desde genHi con Falling Edge Delay (FED) = DEAD_TIME_US e INVERTIDO.
    // Resultado: en cada conmutación, ambos MOSFETs de la pierna permanecen apagados durante DEAD_TIME_US.
    mcpwm_dead_time_config_t dt_hi = {};
    dt_hi.posedge_delay_ticks = DEAD_TIME_US;
    dt_hi.negedge_delay_ticks = 0;
    dt_hi.flags.invert_output = false;

    mcpwm_dead_time_config_t dt_lo = {};
    dt_lo.posedge_delay_ticks = 0;
    dt_lo.negedge_delay_ticks = DEAD_TIME_US;
    dt_lo.flags.invert_output = true;

    if (mcpwm_generator_set_dead_time(s_genPHi, s_genPHi, &dt_hi) != ESP_OK ||
        mcpwm_generator_set_dead_time(s_genPHi, s_genPLo, &dt_lo) != ESP_OK ||
        mcpwm_generator_set_dead_time(s_genNHi, s_genNHi, &dt_hi) != ESP_OK ||
        mcpwm_generator_set_dead_time(s_genNHi, s_genNLo, &dt_lo) != ESP_OK) {
        teardownMcpwmPreludeLocked();
        xSemaphoreGive(s_mcpwmMutex);
        return false;
    }

    // Habilitar e iniciar Timer 0; luego liberar fuerza de genHi (ambos quedan en LOW hasta CMPA_N/CMPB_P)
    if (mcpwm_timer_enable(s_timer) != ESP_OK ||
        mcpwm_timer_start_stop(s_timer, MCPWM_TIMER_START_NO_STOP) != ESP_OK) {
        teardownMcpwmPreludeLocked();
        xSemaphoreGive(s_mcpwmMutex);
        return false;
    }

    mcpwm_generator_set_force_level(s_genPHi, -1, true);
    mcpwm_generator_set_force_level(s_genNHi, -1, true);

#else
    // ========================================================================
    // IMPLEMENTACIÓN ESP-IDF 4.x (Arduino-ESP32 Core 2.x) — driver/mcpwm.h
    // Timer 0 (Pierna P) + Timer 1 (Pierna N) sincronizados por hardware a 50 Hz
    // con Dead-Time AHC por hardware (reloj PWM por defecto 10 MHz -> 100 ns/tick).
    // ========================================================================
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, PIN_VIN1_P_HIN);
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0B, PIN_VIN4_P_LIN);
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1A, PIN_VIN2_N_HIN);
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1B, PIN_VIN3_N_LIN);

    mcpwm_config_t pwm_cfg = {};
    pwm_cfg.frequency = (uint32_t)INVERTER_FREQ_HZ;
    pwm_cfg.cmpr_a = 50.0f;
    pwm_cfg.cmpr_b = 50.0f;
    pwm_cfg.counter_mode = MCPWM_UP_COUNTER;
    pwm_cfg.duty_mode = MCPWM_DUTY_MODE_0;

    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_cfg);
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &pwm_cfg);

    // Dead-Time por hardware: 10 MHz -> 10 ticks = 1 us -> DEAD_TIME_US * 10 ticks
    const uint32_t dt_ticks_10mhz = (uint32_t)DEAD_TIME_US * 10U;
    mcpwm_deadtime_enable(MCPWM_UNIT_0, MCPWM_TIMER_0,
                          MCPWM_ACTIVE_HIGH_COMPLIMENT_MODE, dt_ticks_10mhz, dt_ticks_10mhz);
    mcpwm_deadtime_enable(MCPWM_UNIT_0, MCPWM_TIMER_1,
                          MCPWM_ACTIVE_HIGH_COMPLIMENT_MODE, dt_ticks_10mhz, dt_ticks_10mhz);

    // Sincronismo de fase Timer 0 -> Timer 1 (en milésimas de período: 501 = 180°, 667 = 240° -> alpha=30°)
    uint32_t phase_pm = 500U + (uint32_t)lroundf((alpha / 90.0f) * 500.0f);
    if (phase_pm < 501U) phase_pm = 501U; // Evita colisión exacta con cmpr_a=500 en el instante de sync
    if (phase_pm > 950U) phase_pm = 950U;

    mcpwm_set_timer_sync_output(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_SWSYNC_SOURCE_TEZ);
    mcpwm_sync_config_t sync_cfg = {};
    sync_cfg.sync_sig = MCPWM_SELECT_TIMER0_SYNC;
    sync_cfg.timer_val = phase_pm;
    sync_cfg.count_direction = MCPWM_TIMER_DIRECTION_UP;
    mcpwm_sync_configure(MCPWM_UNIT_0, MCPWM_TIMER_1, &sync_cfg);

    s_legacyRefUs = (uint32_t)esp_timer_get_time();
    s_lastTezUs = s_legacyRefUs;
    s_tezFlag = true;
#endif

    s_currentAlphaDeg = alpha;
    s_running = true;

    xSemaphoreGive(s_mcpwmMutex);
    return true;
}

bool inverterSetAlphaDeg(float alpha_deg) {
    ensureMutexCreated();
    xSemaphoreTake(s_mcpwmMutex, portMAX_DELAY);

    float alpha = clampAlphaDeg(alpha_deg);
    s_currentAlphaDeg = alpha;

    if (!s_running) {
        xSemaphoreGive(s_mcpwmMutex);
        return false;
    }

#if USE_MCPWM_PRELUDE_API
    // Actualización atómica vía registros shadow (se aplica sincronizadamente en el próximo TEZ)
    uint32_t delta = alphaDegToDeltaTicks(alpha);
    mcpwm_comparator_set_compare_value(s_cmpNA, MCPWM_BASE_OFFSET_TICKS + delta);
    mcpwm_comparator_set_compare_value(s_cmpNB, MCPWM_BASE_OFFSET_TICKS + MCPWM_HALF_PERIOD_TICKS + delta);
#else
    uint32_t phase_pm = 500U + (uint32_t)lroundf((alpha / 90.0f) * 500.0f);
    if (phase_pm < 501U) phase_pm = 501U;
    if (phase_pm > 950U) phase_pm = 950U;

    mcpwm_sync_config_t sync_cfg = {};
    sync_cfg.sync_sig = MCPWM_SELECT_TIMER0_SYNC;
    sync_cfg.timer_val = phase_pm;
    sync_cfg.count_direction = MCPWM_TIMER_DIRECTION_UP;
    mcpwm_sync_configure(MCPWM_UNIT_0, MCPWM_TIMER_1, &sync_cfg);
#endif

    xSemaphoreGive(s_mcpwmMutex);
    return true;
}

void inverterEmergencyStop() {
    // Señalizar inmediatamente para abortar cualquier pre-carga en curso
    s_estopRequested = true;
    s_running = false;

    ensureMutexCreated();
    if (xSemaphoreTake(s_mcpwmMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
#if USE_MCPWM_PRELUDE_API
        teardownMcpwmPreludeLocked();
#else
        teardownMcpwmLegacyLocked();
#endif
        xSemaphoreGive(s_mcpwmMutex);
    } else {
        // Respaldo absoluto si el mutex estuviera tomado: forzar los 4 pines directamente a LOW
        forceAllGatePinsOutputLow();
    }
}

bool inverterIsRunning() {
    return s_running;
}

float inverterGetCurrentAlphaDeg() {
    return s_currentAlphaDeg;
}

bool inverterConsumeCycleSync(uint32_t *out_tez_us, uint32_t *out_theta0_offset_us) {
    if (!s_running) {
        return false;
    }

    uint32_t delta = alphaDegToDeltaTicks(s_currentAlphaDeg);

#if USE_MCPWM_PRELUDE_API
    if (!s_tezFlag) {
        return false;
    }
    portENTER_CRITICAL(&g_stateMux);
    s_tezFlag = false;
    uint32_t tez = s_lastTezUs;
    portEXIT_CRITICAL(&g_stateMux);

    // En la arquitectura 1-Timer: el centro del escalón 0 V previo al pulso +VDC
    // está exactamente en t = (MCPWM_BASE_OFFSET_TICKS + 10000) + delta / 2.
    // Alinear ahí hace coincidir theta=0° con la convención de pc_software/calculos.py.
    if (out_tez_us) *out_tez_us = tez;
    if (out_theta0_offset_us) {
        *out_theta0_offset_us = (MCPWM_BASE_OFFSET_TICKS + MCPWM_HALF_PERIOD_TICKS) + (delta / 2U);
    }
    return true;
#else
    uint32_t now = (uint32_t)esp_timer_get_time();
    if ((uint32_t)(now - s_lastTezUs) >= MCPWM_PERIOD_TICKS) {
        uint32_t ciclos = (uint32_t)(now - s_lastTezUs) / MCPWM_PERIOD_TICKS;
        s_lastTezUs += ciclos * MCPWM_PERIOD_TICKS;
        if (out_tez_us) *out_tez_us = s_lastTezUs;
        // En Core 2.x, +VDC inicia en t=0 y dura 10000 - delta; el centro del 0 V previo
        // está en 20000 - delta/2 (equivalente al inicio del siguiente ciclo menos delta/2).
        if (out_theta0_offset_us) {
            *out_theta0_offset_us = (delta > 0U) ? (MCPWM_PERIOD_TICKS - (delta / 2U)) : 0U;
        }
        return true;
    }
    return false;
#endif
}
