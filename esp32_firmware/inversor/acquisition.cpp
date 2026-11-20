#include "acquisition.h"
#include "inverter_mcpwm.h"

#include <esp_timer.h>

// ============================================================================
// BUFFERS INTERNOS Y CALIBRACIÓN
// ============================================================================
static float s_captureV[NUM_MUESTRAS_CICLO] = {0.0f};
static float s_captureI[NUM_MUESTRAS_CICLO] = {0.0f};
static float s_readyV[NUM_MUESTRAS_CICLO]   = {0.0f};
static float s_readyI[NUM_MUESTRAS_CICLO]   = {0.0f};

static float s_acsOffsetPinV = ACS712_OFFSET_PIN_NOM_V;

// Estado de la máquina de muestreo determinístico en Core 1
enum CapturePhase : uint8_t {
    CAP_IDLE = 0,
    CAP_WAIT_THETA0 = 1,
    CAP_SAMPLING = 2
};

static CapturePhase s_phase = CAP_IDLE;
static uint32_t s_targetStartUs = 0;
static uint32_t s_lastCycleActivityUs = 0;
static int s_sampleIdx = 0;

// ============================================================================
// CONVERSIÓN FÍSICA (SOLO ESCALADO LINEAL A VOLTIOS Y AMPERIOS)
// ============================================================================
static inline void sampleOnce(float *out_v, float *out_i) {
    // analogReadMilliVolts() utiliza la calibración de fábrica en eFuse del ESP32
    // para una linealidad superior frente al conteo crudo de 12 bits.
    uint32_t mv_vp   = analogReadMilliVolts(PIN_ADC_VP);
    uint32_t mv_vn   = analogReadMilliVolts(PIN_ADC_VN);
    uint32_t mv_iout = analogReadMilliVolts(PIN_ADC_VIOUT);

    // Tensión diferencial de salida: V_out = (V_adc_vp - V_adc_vn) * 101.0
    float v_p = (float)mv_vp * 0.001f;
    float v_n = (float)mv_vn * 0.001f;
    *out_v = (v_p - v_n) * FACTOR_ESCALA_V;

    // Corriente de salida (ACS712-20A con divisor 1k/2k -> factor 1.5, 100 mV/A)
    float v_i_pin = (float)mv_iout * 0.001f;
    float v_acs_delta = (v_i_pin - s_acsOffsetPinV) * FACTOR_DIVISOR_ACS;
    *out_i = v_acs_delta / ACS712_SENSIBILIDAD_V_A;
}

static void publishCycleAtomically() {
    portENTER_CRITICAL(&g_stateMux);
    for (int k = 0; k < NUM_MUESTRAS_CICLO; k++) {
        s_readyV[k] = s_captureV[k];
        s_readyI[k] = s_captureI[k];
    }
    portEXIT_CRITICAL(&g_stateMux);
}

// ============================================================================
// API PÚBLICA
// ============================================================================

void acquisitionInitPins() {
    pinMode(PIN_ADC_VP, INPUT);
    pinMode(PIN_ADC_VN, INPUT);
    pinMode(PIN_ADC_VIOUT, INPUT);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    analogSetPinAttenuation(PIN_ADC_VP, ADC_11db);
    analogSetPinAttenuation(PIN_ADC_VN, ADC_11db);
    analogSetPinAttenuation(PIN_ADC_VIOUT, ADC_11db);

    s_lastCycleActivityUs = (uint32_t)esp_timer_get_time();
}

void acquisitionCalibrateZeroCurrent() {
    uint64_t sum_mv = 0;
    for (int i = 0; i < ACS712_CAL_MUESTRAS; i++) {
        sum_mv += analogReadMilliVolts(PIN_ADC_VIOUT);
        delayMicroseconds(150);
    }

    float measured_v = ((float)sum_mv / (float)ACS712_CAL_MUESTRAS) * 0.001f;

    if (measured_v >= ACS712_PIN_CAL_MIN_V && measured_v <= ACS712_PIN_CAL_MAX_V) {
        s_acsOffsetPinV = measured_v;
        Serial.printf("[ADC] Calibracion ACS712 (puente OFF): offset en pin = %.4f V (V_acs = %.3f V)\n",
                      s_acsOffsetPinV, s_acsOffsetPinV * FACTOR_DIVISOR_ACS);
    } else {
        s_acsOffsetPinV = ACS712_OFFSET_PIN_NOM_V;
        Serial.printf("[ADC] Aviso: lectura en reposo Viout = %.3f V fuera de [%.2f, %.2f] V "
                      "(posible prueba sin +5V en ACS712). Usando offset nominal %.4f V.\n",
                      measured_v, ACS712_PIN_CAL_MIN_V, ACS712_PIN_CAL_MAX_V, s_acsOffsetPinV);
    }
}

float acquisitionGetAcsOffsetPinV() {
    return s_acsOffsetPinV;
}

void acquisitionPoll() {
    uint32_t now_us = (uint32_t)esp_timer_get_time();

    if (s_phase == CAP_IDLE) {
        uint32_t tez_us = 0;
        uint32_t theta0_offset_us = 0;

        if (inverterConsumeCycleSync(&tez_us, &theta0_offset_us)) {
            // Sincronizado por hardware con el ciclo MCPWM de 50 Hz
            s_targetStartUs = tez_us + theta0_offset_us;
            s_lastCycleActivityUs = now_us;
            s_phase = CAP_WAIT_THETA0;
        } else if ((uint32_t)(now_us - s_lastCycleActivityUs) >= 40000UL) {
            // Modo autónomo (free-running a 50 Hz) cuando el inversor está en STOP
            // para seguir reportando V = 0 V e I = 0 A a la interfaz gráfica.
            s_targetStartUs = now_us;
            s_lastCycleActivityUs = now_us;
            s_sampleIdx = 0;
            s_phase = CAP_SAMPLING;
        }
    }

    if (s_phase == CAP_WAIT_THETA0) {
        // Comparación segura ante desbordamiento de uint32_t (aritmética con signo)
        if ((int32_t)(now_us - s_targetStartUs) >= 0) {
            s_sampleIdx = 0;
            s_phase = CAP_SAMPLING;
        }
    }

    if (s_phase == CAP_SAMPLING) {
        // Instante exacto de la muestra k dentro del ciclo de 20 000 us (0 .. 19 687 us)
        uint32_t expected_offset_us = ((uint32_t)s_sampleIdx * MCPWM_PERIOD_TICKS) / (uint32_t)NUM_MUESTRAS_CICLO;
        uint32_t sample_time_us = s_targetStartUs + expected_offset_us;

        if ((int32_t)(now_us - sample_time_us) >= 0) {
            sampleOnce(&s_captureV[s_sampleIdx], &s_captureI[s_sampleIdx]);
            s_sampleIdx++;

            if (s_sampleIdx >= NUM_MUESTRAS_CICLO) {
                publishCycleAtomically();
                s_lastCycleActivityUs = (uint32_t)esp_timer_get_time();
                s_phase = CAP_IDLE;
            }
        }
    }
}

void acquisitionGetLatestCycle(float out_v[NUM_MUESTRAS_CICLO], float out_i[NUM_MUESTRAS_CICLO]) {
    portENTER_CRITICAL(&g_stateMux);
    for (int k = 0; k < NUM_MUESTRAS_CICLO; k++) {
        out_v[k] = s_readyV[k];
        out_i[k] = s_readyI[k];
    }
    portEXIT_CRITICAL(&g_stateMux);
}
