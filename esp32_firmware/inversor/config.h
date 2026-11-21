#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * ============================================================================
 * Proyecto: InverRect-Monitor - Firmware del Inversor Monofásico (50 Hz)
 * Archivo:  esp32_firmware/inversor/config.h
 * Hardware: PCB/Inversor/Inversor.kicad_pcb (ESP32-DEVKITC-32E + 2x IR2110 + 4x IRF840A)
 *
 * Definición centralizada de pines (verificados contra el PCB de KiCad),
 * parámetros de seguridad anti shoot-through, escalado analógico ADC1 y red.
 * ============================================================================
 */

// ============================================================================
// 1. ASIGNACIÓN DE PINES DE HARDWARE (Verificados en PCB/Inversor/Inversor.kicad_pcb)
// ============================================================================

// Pierna P (U4: IR2110 -> nodo /IP+ -> Q1 superior, Q2 inferior)
// Ambos pines cuentan con pull-down externo de 10 kOhm (R13, R14) en el PCB.
constexpr int PIN_VIN1_P_HIN = 33;  // U8 IO33 -> U4 HIN (pin 10) -> /Vg1 -> Q1 (High-Side P)
constexpr int PIN_VIN4_P_LIN = 32;  // U8 IO32 -> U4 LIN (pin 12) -> /Vg4 -> Q2 (Low-Side P)

// Pierna N (U5: IR2110 -> nodo /VL_n -> Q3 superior, Q4 inferior)
// Ambos pines cuentan con pull-down externo de 10 kOhm (R15, R16) en el PCB.
constexpr int PIN_VIN2_N_HIN = 26;  // U8 IO26 -> U5 HIN (pin 10) -> /Vg2 -> Q3 (High-Side N)
constexpr int PIN_VIN3_N_LIN = 25;  // U8 IO25 -> U5 LIN (pin 12) -> /Vg3 -> Q4 (Low-Side N)

// Selector físico de forma de onda (Jumper J3 entre IO27 y GND)
// HIGH (abierto) = Onda Cuadrada (alpha = 0°)
// LOW  (cerrado) = Onda Cuasicuadrada (alpha = 30°)
constexpr int PIN_WAVE_SELECTOR = 27; // U8 IO27 -> /WS (configurar INPUT_PULLUP)

// Sensado Analógico (ESTRICTAMENTE en ADC1 para compatibilidad total con Wi-Fi)
constexpr int PIN_ADC_VP    = 36;   // U8 SENSOR_VP (ADC1_CH0) -> /ADC_VP (R6=330k / R7=3.3k en /IP+)
constexpr int PIN_ADC_VN    = 35;   // U8 IO35      (ADC1_CH7) -> /ADC_VN (R9=330k / R10=3.3k en /VL_n)
constexpr int PIN_ADC_VIOUT = 39;   // U8 SENSOR_VN (ADC1_CH3) -> /Viout  (ACS712-20A + R11=1k / R12=2k)

// Detector óptico de polaridad de salida (U6: PC817 con pull-up R8=10k a 3.3V)
// LOW cuando V_out > 0 (+VDC en /IP+ respecto a /VL_n), HIGH cuando V_out <= 0.
constexpr int PIN_VZCD = 13;        // U8 IO13 -> /Vzcd (diagnóstico / verificación de salida)

// ============================================================================
// 2. PARÁMETROS DEL INVERSOR, MCPWM Y SEGURIDAD (ANTI SHOOT-THROUGH)
// ============================================================================
#define INVERTER_FREQ_HZ            50.0f
#define MCPWM_RESOLUTION_HZ         1000000UL   // 1 MHz (1 tick = 1 us)
#define MCPWM_PERIOD_TICKS          20000UL     // 20 000 us = 20.000 ms (50.000 Hz)
#define MCPWM_HALF_PERIOD_TICKS     10000UL     // 10 000 us = 180°

// Tiempo muerto por hardware (RED y FED) en microsegundos
#define DEAD_TIME_US                50U
#define DEAD_TIME_MIN_SAFE_US       10U
#define DEAD_TIME_MAX_SAFE_US       500U

// Margen base desde t=0 (TEZ) para evitar que ningún comparador colisione con 0 o 20000
#define MCPWM_BASE_OFFSET_TICKS     600UL

// Verificaciones obligatorias en tiempo de compilación
static_assert(DEAD_TIME_US >= DEAD_TIME_MIN_SAFE_US,
              "SEGURIDAD CRITICA: DEAD_TIME_US debe ser >= 10 us para evitar shoot-through.");
static_assert(DEAD_TIME_US <= DEAD_TIME_MAX_SAFE_US,
              "DEAD_TIME_US excede el maximo permitido (500 us) para 50 Hz.");
static_assert(MCPWM_BASE_OFFSET_TICKS > DEAD_TIME_US,
              "MCPWM_BASE_OFFSET_TICKS debe ser estrictamente mayor que DEAD_TIME_US.");

// Pre-carga de capacitores de bootstrap (C7 y C8 = 22 uF con MUR460 desde +12V)
constexpr uint32_t BOOTSTRAP_PRECHARGE_MS = 5;

// Límites seguros del ángulo de muesca alpha (Phase-Shift Control: conducción = 180° - 2*alpha)
constexpr float ALPHA_MIN_DEG           = 0.0f;   // Onda cuadrada pura (180° de conducción)
constexpr float ALPHA_QUASI_DEFAULT_DEG = 30.0f;  // Onda cuasicuadrada óptima (120°, elimina 3er armónico)
constexpr float ALPHA_MAX_DEG           = 60.0f;  // Mínimo pulso seguro de 60° (conducción 3.33 ms)

// Antirrebote por software del jumper físico WS
constexpr uint32_t WS_DEBOUNCE_MS = 50;

// ============================================================================
// 3. PARÁMETROS DE ADQUISICIÓN Y ESCALADO ADC1 (SIN CÁLCULOS ESTADÍSTICOS EN ESP32)
// ============================================================================
constexpr int NUM_MUESTRAS_CICLO = 64; // 64 muestras / 20 ms = 3200 Hz (compatible con GUI y 1 datagrama UDP)

// Divisores resistivos de tensión diferencial en Pierna P (/IP+) y Pierna N (/VL_n)
// R6 = R9 = 330 kOhm, R7 = R10 = 3.3 kOhm -> Factor = (330k + 3.3k) / 3.3k = 101.0
constexpr float R_DIV_V_SUP_OHM    = 330000.0f;
constexpr float R_DIV_V_INF_OHM    = 3300.0f;
constexpr float FACTOR_ESCALA_V    = (R_DIV_V_SUP_OHM + R_DIV_V_INF_OHM) / R_DIV_V_INF_OHM; // 101.0

// Sensor de corriente ACS712-20A + divisor resistivo R11=1 kOhm (serie) / R12=2 kOhm (a GND)
// Ganancia del divisor = 2/3 -> factor inverso = 1.5
// Offset nominal en pin: 2.50 V * (2/3) = 1.6667 V | Sensibilidad en pin: 66.67 mV/A
constexpr float R_DIV_ACS_SERIE_OHM      = 1000.0f;
constexpr float R_DIV_ACS_GND_OHM        = 2000.0f;
constexpr float FACTOR_DIVISOR_ACS       = (R_DIV_ACS_SERIE_OHM + R_DIV_ACS_GND_OHM) / R_DIV_ACS_GND_OHM; // 1.5
constexpr float ACS712_OFFSET_NOMINAL_V  = 2.50f;
constexpr float ACS712_OFFSET_PIN_NOM_V  = ACS712_OFFSET_NOMINAL_V / FACTOR_DIVISOR_ACS; // ~1.6667 V
constexpr float ACS712_SENSIBILIDAD_V_A  = 0.100f; // 100 mV/A para ACS712xLCTR-20A
constexpr int   ACS712_CAL_MUESTRAS      = 128;    // Muestras promediadas con puente OFF en Safe-Boot

// Ventana de validez del offset calibrado del ACS712 en pin (evita calibración errónea si no hay +5V)
constexpr float ACS712_PIN_CAL_MIN_V = 1.20f;
constexpr float ACS712_PIN_CAL_MAX_V = 2.10f;

// ============================================================================
// 4. CONFIGURACIÓN DE RED WI-FI, UDP SCADA Y ARDUINO OTA (CORE 0)
// ============================================================================
#define WIFI_SSID           "Galaxy A71"      // Configurar con el SSID de la red local
#define WIFI_PASSWORD       "elias3503"  // Configurar con la clave de la red local
#define UDP_PORT            8888                // Puerto UDP compatible con pc_software/interfaz.py
#define OTA_HOSTNAME        "esp32-inversor"    // Hostname exclusivo para OTA (distinto de esp32-rmmoc)
#define OTA_PASSWORD        "epstein"           // Contraseña OTA del proyecto

// ============================================================================
// 5. ESTADO COMPARTIDO ENTRE CORE 0 (RED/OTA) Y CORE 1 (MCPWM/ADC/WS)
// ============================================================================
enum InverterWaveMode : uint8_t {
    WAVE_SQUARE = 0,       // Onda Cuadrada (alpha = 0°)
    WAVE_QUASI_SQUARE = 1  // Onda Cuasicuadrada (alpha configurable, 30° por defecto)
};

struct InverterState {
    bool running;                  // true = MCPWM conmutando; false = Parada de emergencia (all LOW)
    bool estop_latched;            // true = Parada de emergencia activa (requiere START explícito)
    bool control_interfaz_activo;  // true = Obedece comandos UDP; false = Obedece jumper físico WS
    bool jumper_ws_closed;         // true = Jumper WS a GND (Cuasicuadrada); false = Abierto (Cuadrada)
    InverterWaveMode wave_mode;    // Modo de onda activo
    float alpha_deg;               // Ángulo alpha efectivo (0.0° a 60.0°)
};

// Spinlock compartido para lectura/escritura atómica del estado y doble buffer ADC
extern portMUX_TYPE g_stateMux;
extern volatile InverterState g_inverterState;
