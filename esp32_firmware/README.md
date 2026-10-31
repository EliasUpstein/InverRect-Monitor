# Firmware ESP32 - Control de Disparo Único, Selección de Onda, ADC y Comunicaciones en Red

Este directorio contiene el firmware embebido para el microcontrolador **ESP32** (SoC dual-core Xtensa LX6 a 240 MHz), diseñado para el control de fase en tiempo real de convertidores estáticos de potencia mediante un **único circuito de disparo para ambos tiristores**.

Permite conmutar dinámicamente la salida entre **Onda Completa** (disparo en todos los cruces por cero) y **Media Onda** (disparo intercalado en un único semiciclo por período), priorizando las órdenes de la interfaz gráfica SCADA cuando existe conexión inalámbrica y conmutando automáticamente a los controles físicos de hardware (potenciómetro ADC y pin con pull-up) en caso de desconexión o fallo de red.

---

## 1. Arquitectura Dual-Core (FreeRTOS)

El firmware desacopla las operaciones de tiempo crítico de las tareas de red y servicios auxiliares aprovechando los dos núcleos del ESP32:

```
                  ┌─────────────────────────────────────────────────────────┐
                  │                    ESP32 SoC (240 MHz)                  │
                  └─────────────────────────────────────────────────────────┘
                                   │                       │
                 ┌─────────────────┴─────────┐   ┌─────────┴─────────────────┐
                 │          CORE 1           │   │          CORE 0           │
                 │   (Tiempo Real Estricto)  │   │   (Comunicaciones / Red)  │
                 ├───────────────────────────┤   ├───────────────────────────┤
  Opto ZCD ─────►│ • ISR Cruce por Cero      │   │ • Conexión Wi-Fi (STA)    │
  (GPIO 4)       │   (CHANGE - cada 10 ms)   │   │ • Servidor UDP (Port 8888)│
                 │ • Onda Completa (Todos)   │   │ • Servicio ArduinoOTA     │
  Potenciómetro ─►│   vs Media Onda (Interc.) │   │   con suspensión segura   │
  (GPIO 36 ADC)  │ • Prioridad GUI / Fallback│   │   de potencia en onStart  │
  Pin Pull-Up ──►│   a Hardware (GPIO 36/17) │   │ • Parseo de Ángulo y Onda │
  (GPIO 17)      │ • Disparo de Compuerta    │   │   ("ALFA:..;ONDA:..")     │
  MOC3021 ◄──────│   Único (Pulso de 50 µs)  │   │ • Sincronización atómica  │
  (GPIO 16)      └─────────────┬─────────────┘   └─────────────┬─────────────┘
                               │                               │
                               └───────────► MUX ◄─────────────┘
                                    (timerMux - Spinlock)
```

### Core 1: Control de Tiempo Real, Selección de Onda y Fallback de Hardware
* **Detección de Todos los Cruces por Cero (`isrCrucePorCero`):**
  * Vinculada a interrupción en ambos flancos (`CHANGE`) en **GPIO 4**, detectando cada semiciclo ($180^\circ$, $\sim 10000\,\mu\text{s}$ a 50 Hz).
  * **Filtro Antirrebote Industrial:** Descarta ruidos y transitorios menores a $6000\,\mu\text{s}$ (`FILTRO_DEBOUNCE_ZCD_US`).
* **Circuito de Disparo Único (Onda Completa vs Media Onda):**
  * **Onda Completa (`modo_onda_completa = true`):** Utiliza **todos** los cruces por cero para emitir el pulso de disparo en ambos semiciclos hacia el opto-TRIAC (`MOC3021`), cebando alternadamente ambos tiristores.
  * **Media Onda (`modo_onda_completa = false`):** Utiliza los cruces por cero de manera **intercalada** (`semicicloIntercalado = !semicicloIntercalado`), disparando únicamente en un semiciclo por cada ciclo de red.
* **Lógica de Prioridad (Interfaz Inalámbrica vs Hardware Local):**
  * **Con conexión activa (`control_interfaz_activo = true`):** Obedece la configuración enviada desde la interfaz gráfica (tipo de onda y ángulo fijo o lectura de ADC si se seleccionó `11. Muestras ADC`).
  * **Sin conexión / Fallo Wi-Fi (`control_interfaz_activo = false`):** Evalúa automáticamente el hardware en cada ciclo:
    1. **Potenciómetro Analógico (GPIO 36):** Mapea la lectura ADC de 12 bits ($0-4095$) al rango seguro de $5.0^\circ$ a $175.0^\circ$.
    2. **Selector de Onda por Hardware (GPIO 17 con Pull-Up):** Nivel alto (`HIGH`) selecciona **Onda Completa** y nivel bajo (`LOW`) selecciona **Media Onda**.

---

## 2. Asignación de Pines (Pinout de Hardware)

| Función | Pin ESP32 | Tipo | Dispositivo Conectado | Descripción |
| :--- | :---: | :---: | :--- | :--- |
| **Cruce por Cero (ZC)** | `GPIO 4` | Entrada Digital | Optoacoplador (PC817) | Detección de cada semiciclo (`CHANGE`, cada 10 ms) con filtro de 6 ms. |
| **Disparo Compuerta Único** | `GPIO 16` | Salida Digital | Driver Opto-TRIAC (MOC3021) | Pulso de $50\,\mu\text{s}$ hacia el circuito de disparo único de ambos SCRs. |
| **Selector de Onda HW** | `GPIO 17` | Entrada Pull-Up | Jumper / Switch con Pull-Up | Fallback sin Wi-Fi: `HIGH` = Onda Completa, `LOW` = Media Onda. |
| **Potenciómetro Analógico** | `GPIO 36` | Entrada Analógica | Potenciómetro (ADC1 Channel 0 / VP) | Variación analógica del ángulo $\alpha$ ($5^\circ$ a $175^\circ$). |
| **Monitor Serie (UART)** | `TX0 / RX0` | Bidireccional | PC / Convertidor USB-Serie | Telemetría a 115200 baudios y comandos de prueba (`ADC`, `COMPLETA`, `MEDIA`, `HW`). |

---

## 3. Protocolo de Comunicación UDP (Ángulo + Selección de Onda)

El ESP32 escucha en el puerto UDP `8888`:

```
[PC -> ESP32]   ALFA:60.0;ONDA:COMPLETA      --> Ángulo fijo 60.0° y salida en Onda Completa
[ESP32 -> PC]   ACK:ALFA:60.0;ONDA:COMPLETA  --> Confirmación de parámetros aplicados

[PC -> ESP32]   ALFA:45.0;ONDA:MEDIA         --> Ángulo fijo 45.0° y salida en Media Onda (intercalada)
[ESP32 -> PC]   ACK:ALFA:45.0;ONDA:MEDIA     --> Confirmación de parámetros aplicados

[PC -> ESP32]   ALFA:ADC;ONDA:COMPLETA       --> Ángulo por potenciómetro ADC y salida en Onda Completa
[ESP32 -> PC]   ACK:ALFA:ADC:<deg>;ONDA:COMPLETA
```
