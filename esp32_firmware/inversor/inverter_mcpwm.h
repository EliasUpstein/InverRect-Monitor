#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * ============================================================================
 * Módulo: inverter_mcpwm
 * Responsabilidad:
 *  - Arranque seguro (Safe-Boot): fuerza Vin1..Vin4 en LOW antes que cualquier periférico.
 *  - Pre-carga segura de capacitores de bootstrap (C7, C8) encendiendo solo Low-Side.
 *  - Generación 100% por hardware (periférico MCPWM del ESP32) a 50 Hz con
 *    Phase-Shift Control y Dead-Time por hardware (RED/FED >= 10 us, 50 us default).
 *  - Detección automática del Core ESP32 (IDF 5.x mcpwm_prelude vs IDF 4.x mcpwm).
 *  - Parada de emergencia inmediata (inverterEmergencyStop) para fail-safe y OTA.
 * ============================================================================
 */

// Paso 1 (primerísimas líneas de setup()): fuerza los 4 pines de compuerta a OUTPUT LOW
void inverterSafeBootPinsLow();

// Inicializa recursos de sincronización y arranca el puente (pre-carga bootstrap + MCPWM)
bool inverterStart(float initial_alpha_deg);

// Actualiza el ángulo alpha (0.0° a 60.0°) sin glitches ni pérdida de dead-time
bool inverterSetAlphaDeg(float alpha_deg);

// Parada de emergencia inmediata: apaga los 4 MOSFETs (Vin1..Vin4 = LOW) por hardware
void inverterEmergencyStop();

// Indica si el MCPWM está activo conmutando el puente
bool inverterIsRunning();

// Devuelve el ángulo alpha actualmente cargado en el hardware MCPWM
float inverterGetCurrentAlphaDeg();

// Devuelve el último timestamp (micros / esp_timer_get_time) del inicio de ciclo (TEZ)
// y limpia la bandera de nuevo ciclo si había un evento pendiente.
bool inverterConsumeCycleSync(uint32_t *out_tez_us, uint32_t *out_theta0_offset_us);
