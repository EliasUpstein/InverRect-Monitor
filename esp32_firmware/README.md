# Firmware ESP32 - Rectificador Controlado e Inversor Monofásico

Este directorio contiene los dos proyectos de firmware embebido para **ESP32** (compatible con **Arduino IDE** y PlatformIO), organizados en carpetas independientes cuyo nombre coincide exactamente con el archivo `.ino` principal según la convención de Arduino IDE:

```
esp32_firmware/
├── rectificador/
│   └── rectificador.ino     # Rectificador Monofásico (Media Onda / Onda Completa, ZCD + SCR + ADC)
├── inversor/
│   ├── inversor.ino         # Sketch principal del Inversor Monofásico (50 Hz, Puente H MCPWM + ADC1 + OTA)
│   ├── config.h             # Pines verificados del PCB, límites de seguridad, escalado ADC1 y Wi-Fi/OTA
│   ├── inverter_mcpwm.h/.cpp# Generación 100% hardware MCPWM (Phase-Shift + Dead-Time 50 µs + E-Stop)
│   ├── acquisition.h/.cpp   # Calibración ACS712 y muestreo ADC1 (64 muestras/ciclo, sin cálculos estadísticos)
│   └── comms.h/.cpp         # Core 0: Wi-Fi STA, ArduinoOTA seguro y servidor UDP SCADA (puerto 8888)
└── README.md                # Este documento
```

En ambos convertidores, el ESP32 **no realiza cálculos estadísticos ni espectrales** ($V_{\text{rms}}, I_{\text{rms}}, P, Q, S, D, \text{THD}$): únicamente controla la etapa de potencia en tiempo real y envía los vectores muestreados de tensión `V[64]` y corriente `I[64]` vía UDP hacia `pc_software/interfaz.py` para su análisis en Python (`calculos.py`).

---

## Parte A: Firmware del Rectificador (`rectificador/rectificador.ino`)

### 1. Arquitectura Dual-Core (FreeRTOS)
* **Core 1 (Tiempo Real):** ISR de cruce por cero (`GPIO 4`, `CHANGE`, filtro antirrebote 6 ms), pulso de disparo único (`GPIO 16`, 50 µs), muestreo síncrono de 64 muestras/ciclo en `D35` ($V$) y `D34` ($I$), y fallback automático a hardware local (`D36` potenciómetro + `D25` selector de onda).
* **Core 0 (Red y OTA):** Wi-Fi STA, `ArduinoOTA` (hostname `esp32-rmmoc`, apaga el disparo en `onStart`) y servidor UDP en puerto `8888`.

### 2. Pinout del Rectificador (`PCB/RMOC-SC/RMOC-SC.kicad_pcb`)

| Función | Pin ESP32 | Tipo | Circuito Conectado | Descripción |
| :--- | :---: | :---: | :--- | :--- |
| **Cruce por Cero (ZC)** | `GPIO 4` (`D4`) | Entrada Digital | Optoacoplador (PC817) | Detección de cada semiciclo (`CHANGE`, cada 10 ms) con filtro de 6 ms. |
| **Disparo Compuerta Único** | `GPIO 16` (`D16`) | Salida Digital | Driver Opto-TRIAC (MOC3021) | Pulso de $50\,\mu\text{s}$ hacia el circuito de disparo único de ambos SCRs. |
| **Selector de Onda HW** | `GPIO 25` (`D25`) | Entrada Pull-Up | Selector con Pull-Up | Fallback sin Wi-Fi: `HIGH` = Onda Completa, `LOW` = Media Onda. |
| **ADC Corriente Salida** | `GPIO 34` (`D34`) | Entrada `ADC1_CH6` | `ACS712` + Divisor ($1\text{ k}\Omega / 2\text{ k}\Omega$) | Medición de corriente adaptada al rango de $3.3\text{ V}$. |
| **ADC Tensión Salida** | `GPIO 35` (`D35`) | Entrada `ADC1_CH7` | Divisor resistivo + offset $1.65\text{ V}$ | Medición de tensión instantánea de salida del rectificador. |
| **Potenciómetro Analógico** | `GPIO 36` (`VP`) | Entrada `ADC1_CH0` | Potenciómetro de control | Variación analógica del ángulo $\alpha$ ($5^\circ$ a $175^\circ$). |

### 3. Protocolo UDP del Rectificador (Puerto `8888`)
```text
[PC -> ESP32]   ALFA:60.0;ONDA:COMPLETA      --> Ángulo fijo 60.0° y Onda Completa
[PC -> ESP32]   ALFA:45.0;ONDA:MEDIA         --> Ángulo fijo 45.0° y Media Onda
[PC -> ESP32]   ALFA:ADC;ONDA:COMPLETA       --> Ángulo controlado por potenciómetro D36
[ESP32 -> PC]   ACK:ALFA:60.0;ONDA:COMPLETA  --> Confirmación
[PC -> ESP32]   GET_ADC                      --> Solicitud de vectores V[64] e I[64]
[ESP32 -> PC]   ADC_DATA:ALFA:<deg>;ONDA:<COMPLETA|MEDIA>;V:<v0..v63>;I:<i0..i63>
```

---

## Parte B: Firmware del Inversor Monofásico (`inversor/inversor.ino`)

Diseñado para el hardware **Puente H completo** (`4x IRF840A`, `2x IR2110`, bus DC ~311 V, 50 Hz, $P_{\text{out}} \ge 220\text{ W}$) definido en `PCB/Inversor/Inversor.kicad_pcb`.

### 1. Arquitectura Dual-Core y Seguridad Anti Shoot-Through

```
                    ┌───────────────────────────────────────────────────────────┐
                    │                ESP32-DEVKITC-32E (240 MHz)                │
                    └─────────────────────────────┬─────────────────────────────┘
                             ┌────────────────────┴────────────────────┐
                             ▼                                         ▼
              ┌─────────────────────────────┐           ┌─────────────────────────────┐
              │       CORE 1 (Potencia)     │           │      CORE 0 (Red / OTA)     │
              ├─────────────────────────────┤           ├─────────────────────────────┤
  WS (IO27) ─►│ • MCPWM Hardware 50 Hz      │           │ • Wi-Fi STA + Reconexión    │
              │   1 Timer + 2 Operadores    │           │   (fallback automático WS)  │
  ADC_VP ────►│   Phase-Shift (0° .. 60°)   │           │ • ArduinoOTA (esp32-inversor│
  (IO36)      │   Dead-Time HW: 50 µs RED/  │           │   con inverterEmergencyStop │
  ADC_VN ────►│   FED (static_assert >=10µs)│           │   obligatorio en onStart()) │
  (IO35)      │ • Safe-Boot + Calibración   │           │ • Servidor UDP Puerto 8888  │
  Viout ─────►│   ACS712 + Pre-carga 5 ms   │           │   GET_ADC / ALFA / ONDA /   │
  (IO39)      │ • Adquisición ADC1 64 m/c   │◄── MUX ──►│   HW / STOP / START / STATUS│
              └──────────────┬──────────────┘           └─────────────────────────────┘
                             │ Vin1(IO33), Vin4(IO32) -> U4 IR2110 (Pierna P)
                             │ Vin2(IO26), Vin3(IO25) -> U5 IR2110 (Pierna N)
                             ▼
```

* **Detección Automática de API MCPWM:** En compilación detecta `ESP_IDF_VERSION >= 5.0.0` (Arduino-ESP32 Core 3.x → `driver/mcpwm_prelude.h`) o versiones anteriores (Arduino-ESP32 Core 2.x → `driver/mcpwm.h`).
* **Phase-Shift Control (50% duty permanente por pierna):**
  * **Onda Cuadrada ($\alpha = 0^\circ$):** Pierna P y Pierna N desfasadas $180^\circ$ ($10\text{ ms}$).
  * **Onda Cuasicuadrada ($\alpha = 30^\circ$ por defecto, ajustable $0^\circ\text{–}60^\circ$):** Desfase $180^\circ - 2\alpha = 120^\circ$ ($6.667\text{ ms}$), generando pulsos simétricos de $120^\circ$ y escalones activos de $0\text{ V}$ de $60^\circ$ (camino de rueda libre inductiva, eliminación del 3er armónico y recarga continua de los capacitores de bootstrap `C7/C8`).
* **Secuencia Safe-Boot:** En la línea 1 de `setup()` fuerza `Vin1..Vin4 = LOW`, calibra el offset en reposo del `ACS712` (puente apagado), realiza pre-carga de bootstrap encendiendo solo `Vin4` y `Vin3` durante `5 ms` (con `Vin1 = Vin2 = LOW`), respeta `DEAD_TIME_US` y recién entonces habilita el MCPWM.
* **Parada de Emergencia (`inverterEmergencyStop()`):** Deshabilita el bloque dead-time/inversión AHC y el timer MCPWM y fuerza los 4 pines (`33, 32, 26, 25`) a `OUTPUT LOW` de inmediato (invocada por comando `STOP` o en `ArduinoOTA.onStart()`).

### 2. Pinout del Inversor (`PCB/Inversor/Inversor.kicad_pcb` — `U8: ESP32-DEVKITC-32E`)

| Señal PCB | Pin ESP32 | GPIO | Tipo | Circuito Conectado | Descripción |
| :--- | :---: | :---: | :--- | :--- | :--- |
| **`Vin1`** | `J2_8` | `GPIO 33` | Salida MCPWM | `U4` pin 10 (`HIN`) + `R13` 10k PD | Compuerta `Q1` (High-Side Pierna P). |
| **`Vin4`** | `J2_7` | `GPIO 32` | Salida MCPWM | `U4` pin 12 (`LIN`) + `R14` 10k PD | Compuerta `Q2` (Low-Side Pierna P). |
| **`Vin2`** | `J2_10` | `GPIO 26` | Salida MCPWM | `U5` pin 10 (`HIN`) + `R15` 10k PD | Compuerta `Q3` (High-Side Pierna N). |
| **`Vin3`** | `J2_9` | `GPIO 25` | Salida MCPWM | `U5` pin 12 (`LIN`) + `R16` 10k PD | Compuerta `Q4` (Low-Side Pierna N). |
| **`WS`** | `J2_11` | `GPIO 27` | `INPUT_PULLUP` | Jumper `J3` a `GND` | `HIGH` (abierto) = Cuadrada ($0^\circ$); `LOW` (cerrado) = Cuasicuadrada ($30^\circ$). |
| **`ADC_VP`** | `J2_3` | `GPIO 36` (`VP`) | `ADC1_CH0` | `R6=330k / R7=3.3k` en `/IP+` | Tensión Pierna P ($\times 101$). |
| **`ADC_VN`** | `J2_6` | `GPIO 35` | `ADC1_CH7` | `R9=330k / R10=3.3k` en `/VL_n` | Tensión Pierna N ($\times 101$). $V_{\text{out}} = (V_P - V_N)\cdot 101$. |
| **`Viout`** | `J2_4` | `GPIO 39` (`VN`) | `ADC1_CH3` | `ACS712-20A` + `R11=1k / R12=2k` | Corriente de carga ($\text{factor } 1.5$, sensibilidad efectiva $66.67\text{ mV/A}$). |

*(El pin `GPIO 13` / `Vzcd` no es utilizado por el firmware del inversor; tanto la conmutación de 50 Hz como el muestreo ADC1 son 100% autónomos y sincronizados al timer MCPWM interno).*

### 3. Protocolo UDP del Inversor (Puerto `8888`)
```text
[PC -> ESP32]   ALFA:0.0;ONDA:CUADRADA            --> Modo Onda Cuadrada (α = 0.0°)
[PC -> ESP32]   ALFA:30.0;ONDA:CUASICUADRADA      --> Modo Cuasicuadrada (α en [0°..60°])
[ESP32 -> PC]   ACK:ALFA:30.0;ONDA:CUASICUADRADA  --> Confirmación
[PC -> ESP32]   HW                                --> Cede control al jumper físico WS (GPIO 27)
[ESP32 -> PC]   ACK:MODO:HW;ALFA:30.0;ONDA:CUASICUADRADA
[PC -> ESP32]   STOP                              --> Parada de emergencia inmediata (Vin1..Vin4 = LOW)
[ESP32 -> PC]   ACK:STOP
[PC -> ESP32]   START                             --> Reanuda con pre-carga bootstrap + MCPWM
[ESP32 -> PC]   ACK:START;ALFA:30.0;ONDA:CUASICUADRADA
[PC -> ESP32]   GET_ADC                           --> Solicitud de vectores V[64] e I[64]
[ESP32 -> PC]   ADC_DATA:ALFA:<deg>;ONDA:<CUADRADA|CUASICUADRADA|STOP>;V:<v0..v63>;I:<i0..i63>
```

---

## Parte C: Verificación Segura en Osciloscopio del Inversor (SIN Alta Tensión)

> **IMPORTANTE:** Antes de conectar el bus de 220 V AC / 311 V DC (`J1`), realizá **siempre** esta verificación completa alimentando únicamente la etapa lógica (**3.3 V / 5 V** del ESP32, y opcionalmente **+12 V** para probar los `IR2110` sin potencia en `VDC`).

### Paso 1: Configuración y Carga Inicial
1. Abrir `esp32_firmware/inversor/inversor.ino` en Arduino IDE.
2. Configurar `WIFI_SSID` y `WIFI_PASSWORD` en `esp32_firmware/inversor/config.h`.
3. Flashear el ESP32 por USB (las actualizaciones posteriores podrán hacerse por **ArduinoOTA** seleccionando `esp32-inversor`).
4. Abrir el Monitor Serie a **115200 baudios** y verificar el arranque:
   * Calibración del `ACS712` (si no hay +5 V en el PCB, advertirá y usará el offset nominal `1.6667 V`).
   * Confirmación `MCPWM 50 Hz ACTIVO` y dirección IP asignada.

### Paso 2: Verificación de Complementariedad y Dead-Time (50 µs) por Pierna
Conectar las puntas del osciloscopio a los testpoints del PCB (masa en `GND`):
1. **Pierna P (`TP8 = Vin1 / IO33` en CH1, `TP7 = Vin4 / IO32` en CH2):**
   * Escala horizontal `5 ms/div`, vertical `2 V/div`.
   * Comprobar que ambas son ondas cuadradas limpias de **50.0 Hz (`T = 20.0 ms`)**, amplitud `3.3 V`, ciclo de trabajo ~50% y **complementarias** (nunca en `HIGH` a la vez).
   * Hacer zoom a **`20 µs/div`** con trigger por flanco descendente en CH1 (`Vin1`), y luego en CH2 (`Vin4`):
     * Confirmar que cuando `Vin1` cae a `0 V`, `Vin4` tarda exactamente **`50 µs`** en subir a `3.3 V`.
     * Confirmar que cuando `Vin4` cae a `0 V`, `Vin1` tarda exactamente **`50 µs`** en subir a `3.3 V`.
     * **Criterio de aprobación:** En ambos flancos existe una ventana de **`50 µs`** donde `Vin1 = 0 V` y `Vin4 = 0 V` simultáneamente (cero solapamiento).
2. **Pierna N (`TP6 = Vin2 / IO26` en CH1, `TP5 = Vin3 / IO25` en CH2):**
   * Repetir exactamente la misma medición de 50 Hz, complementariedad y **`50 µs`** de tiempo muerto en ambos flancos.

### Paso 3: Verificación de Desfase (Cuadrada vs. Cuasicuadrada) con Jumper `WS`
Conectar **CH1 a `TP8 (Vin1)`** y **CH2 a `TP6 (Vin2)`** (y activar función matemática **`CH1 - CH2`** en el osciloscopio para visualizar directamente la forma de onda que verá la carga):
1. **Jumper `J3 (WS)` ABIERTO (`HIGH`):**
   * `Vin1` y `Vin2` deben estar en **oposición de fase exacta ($180^\circ = 10.0\text{ ms}$)**.
   * La resta matemática `CH1 - CH2` muestra una **onda cuadrada pura** (`+3.3 V` durante `10 ms`, `-3.3 V` durante `10 ms`).
2. **Cerrar Jumper `J3 (WS)` a `GND` (`LOW`):**
   * El cambio ocurre en < 50 ms sin cortes ni glitches en el dead-time.
   * `Vin2` se desplaza respecto a `Vin1` a **`120°` (`6.67 ms`)**.
   * La resta matemática `CH1 - CH2` muestra una **onda cuasicuadrada simétrica**: `+3.3 V` durante `6.67 ms` ($120^\circ$), `0 V` durante `3.33 ms` ($60^\circ$), `-3.3 V` durante `6.67 ms` ($120^\circ$) y `0 V` durante `3.33 ms` ($60^\circ$).
3. **Verificación de Parada de Emergencia y Control Remoto:**
   * Enviar `STOP` por Monitor Serie o presionar **`STOP (Emergencia)`** en `pc_software/interfaz.py` (seleccionando `Dispositivo: Inversor`): confirmar en el osciloscopio que **`Vin1`, `Vin2`, `Vin3` y `Vin4` caen inmediatamente a `0.0 V`**.
   * Enviar `START` (o presionar **`START (Reanudar)`**): verificar el pulso de pre-carga de `5 ms` solo en `Vin4` y `Vin3` (con `Vin1 = Vin2 = 0 V`) seguido del arranque limpio de 50 Hz.
   * Probar desde la GUI valores de $\alpha$ (ej. `15°`, `30°`, `45°`) en modo `Cuasi-cuadrada`, luego `Jumper Físico (HW)` y finalmente seleccionar `11. Muestras ADC` para verificar la recepción de las 64 muestras de `V` e `I` por UDP.
