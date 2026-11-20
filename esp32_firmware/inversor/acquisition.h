#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/**
 * ============================================================================
 * Módulo: acquisition
 * Responsabilidad:
 *  - Configuración de los 3 canales de ADC1 (GPIO 36, 35, 39) con atenuación 11 dB.
 *  - Calibración del offset en reposo (0 A) del sensor ACS712-20A con el puente OFF.
 *  - Muestreo determinístico de 64 puntos por ciclo de 50 Hz (3200 Hz) sincronizado
 *    con el timer MCPWM y alineado a theta = 0° (convención de pc_software/calculos.py).
 *  - Publicación atómica mediante doble buffer para Core 0 (UDP).
 *  - NO realiza cálculos estadísticos ni espectrales (se calculan en Python).
 * ============================================================================
 */

// Configura los pines ADC1
void acquisitionInitPins();

// Calibra el offset en reposo del ACS712 (llamar SOLO con el puente H en LOW / 0 A)
void acquisitionCalibrateZeroCurrent();

// Devuelve el offset calibrado del ACS712 en el pin GPIO 39 (en Voltios)
float acquisitionGetAcsOffsetPinV();

// Ejecuta la máquina de estados no bloqueante de muestreo ADC1 (llamar desde loop() en Core 1)
void acquisitionPoll();

// Copia de forma atómica el último ciclo completo de V[64] e I[64] hacia buffers del llamador (Core 0)
void acquisitionGetLatestCycle(float out_v[NUM_MUESTRAS_CICLO], float out_i[NUM_MUESTRAS_CICLO]);
