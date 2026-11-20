#pragma once

#include "config.h"

/**
 * ============================================================================
 * Módulo: comms
 * Responsabilidad (Core 0 - FreeRTOS TaskComunicaciones):
 *  - Conexión y reconexión automática Wi-Fi STA (con fallback al jumper WS ante caída).
 *  - Servicio ArduinoOTA con parada de seguridad obligatoria (inverterEmergencyStop) en onStart().
 *  - Servidor UDP en puerto 8888 compatible con pc_software/interfaz.py:
 *      * GET_ADC / LEER_ADC -> ADC_DATA:ALFA:<deg>;ONDA:<CUADRADA|CUASICUADRADA|STOP>;V:...;I:...
 *      * ALFA:<deg>;ONDA:<CUADRADA|CUASICUADRADA> -> ACK / NACK
 *      * HW / MODO:HW -> Devuelve el control al jumper físico WS (GPIO 27)
 *      * STOP / ESTOP -> Parada de emergencia inmediata (4 MOSFETs en LOW)
 *      * START        -> Reanuda con pre-carga segura de bootstrap + MCPWM
 *      * STATUS       -> Diagnóstico en texto plano
 * ============================================================================
 */

// Lanza la tarea FreeRTOS de comunicaciones (Wi-Fi + UDP + ArduinoOTA) fijada al Core 0
void commsStartTaskOnCore0();

// Utilidad compartida para aplicar un comando de estado (también usada por Serial en Core 1)
bool commsApplyGuiCommand(float alpha_deg, InverterWaveMode mode, bool from_gui);
