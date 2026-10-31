# Firmware ESP32 - Control de Disparo Único, Selección de Onda, Lecturas ADC (Tensión/Corriente) y Comunicaciones en Red

Este directorio contiene el firmware embebido para el microcontrolador **ESP32** (SoC dual-core Xtensa LX6 a 240 MHz), diseñado para el control de fase en tiempo real y adquisición de formas de onda de convertidores estáticos de potencia mediante un **único circuito de disparo para ambos tiristores**.

Permite:
1. Conmutar dinámicamente la salida entre **Onda Completa** (disparo en todos los cruces por cero) y **Media Onda** (disparo intercalado en un único semiciclo por período), priorizando las órdenes de la interfaz gráfica cuando existe conexión inalámbrica y conmutando automáticamente a los controles físicos de hardware (potenciómetro en `D36` y selector con pull-up en `D25`) en caso de desconexión.
2. Adquirir de forma sincronizada con el cruce por cero un ciclo completo ($360^\circ$, 64 muestras) de la **Tensión de Salida (`D35`)** mediante divisor resistivo y la **Corriente de Salida (`D34`)** mediante sensor **ACS712** y su divisor resistivo, enviando ambas formas de onda reales a la interfaz gráfica vía UDP.

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
  Divisor Vout ─►│ • Muestreo Síncrono ADC   │   │ • Servicio ArduinoOTA     │
  (GPIO 35 / D35)│   64 muestras/ciclo V e I │   │   con suspensión segura   │
  ACS712 Iout ──►│ • Prioridad al Disparo    │   │   de potencia en onStart  │
  (GPIO 34 / D34)│   (guardia de 80 µs)      │   │ • Comandos "ALFA:..;ONDA" │
  Potenciómetro ►│ • Fallback HW (D36 / D25) │   │   y Telemetría "GET_ADC"  │
  (GPIO 36 / D36)│ • Disparo de Compuerta    │   │ • Sincronización atómica  │
  Pull-Up Onda ─►│   Único (Pulso de 50 µs)  │   │   mediante Spinlock MUX   │
  (GPIO 25 / D25)└─────────────┬─────────────┘   └─────────────┬─────────────┘
  MOC3021 ◄──────┘             │                               │
  (GPIO 16)                    └───────────► MUX ◄─────────────┘
                                    (timerMux - Spinlock)
```

---

## 2. Asignación de Pines (Pinout de Hardware)

| Función | Pin ESP32 | Tipo | Circuito Conectado | Descripción |
| :--- | :---: | :---: | :--- | :--- |
| **Cruce por Cero (ZC)** | `GPIO 4` (`D4`) | Entrada Digital | Optoacoplador (PC817) | Detección de cada semiciclo (`CHANGE`, cada 10 ms) con filtro de 6 ms. |
| **Disparo Compuerta Único** | `GPIO 16` (`D16`) | Salida Digital | Driver Opto-TRIAC (MOC3021) | Pulso de $50\,\mu\text{s}$ hacia el circuito de disparo único de ambos SCRs. |
| **Selector de Onda HW** | `GPIO 25` (`D25`) | Entrada Pull-Up | Selector con Pull-Up | Fallback sin Wi-Fi: `HIGH` = Onda Completa, `LOW` = Media Onda. |
| **ADC Corriente Salida** | `GPIO 34` (`D34`) | Entrada Analógica (`ADC1_CH6`) | Salida `ACS712` + Divisor ($1\text{ k}\Omega / 2\text{ k}\Omega$) | Medición de corriente de salida adaptada al rango de $3.3\text{ V}$ del ESP32. |
| **ADC Tensión Salida** | `GPIO 35` (`D35`) | Entrada Analógica (`ADC1_CH7`) | Divisor resistivo de salida + offset $1.65\text{ V}$ | Medición de tensión instantánea de salida del rectificador. |
| **Potenciómetro Analógico** | `GPIO 36` (`VP`) | Entrada Analógica (`ADC1_CH0`) | Potenciómetro de control | Variación analógica del ángulo $\alpha$ ($5^\circ$ a $175^\circ$). |
| **Monitor Serie (UART)** | `TX0 / RX0` | Bidireccional | PC / Convertidor USB-Serie | Telemetría a 115200 baudios y comandos de prueba (`ADC`, `COMPLETA`, `MEDIA`, `HW`, `MUESTRAS`). |

---

## 3. Acondicionamiento Analógico y Escalado ADC

* **Tensión de Salida (`D35` / `GPIO 35`):**
  $$V_{\text{out}} = \left(V_{\text{ADC,D35}} - V_{\text{offset,1.65V}}\right) \cdot \frac{R_{\text{sup}} + R_{\text{inf}}}{R_{\text{inf}}}$$
* **Corriente de Salida (`D34` / `GPIO 34` - Sensor ACS712):**
  $$V_{\text{ACS712}} = V_{\text{ADC,D34}} \cdot \frac{R_{\text{serie}} + R_{\text{gnd}}}{R_{\text{gnd}}}, \qquad I_{\text{out}} = \frac{V_{\text{ACS712}} - 2.50\text{ V}}{\text{Sensibilidad}_{\text{ACS712}}}$$

---

## 4. Protocolo de Comunicación UDP (Puerto `8888`)

```
[PC -> ESP32]   ALFA:60.0;ONDA:COMPLETA      --> Ángulo fijo 60.0° y salida en Onda Completa
[ESP32 -> PC]   ACK:ALFA:60.0;ONDA:COMPLETA  --> Confirmación de parámetros aplicados

[PC -> ESP32]   ALFA:45.0;ONDA:MEDIA         --> Ángulo fijo 45.0° y salida en Media Onda (intercalada)
[ESP32 -> PC]   ACK:ALFA:45.0;ONDA:MEDIA     --> Confirmación de parámetros aplicados

[PC -> ESP32]   GET_ADC                      --> Solicitud de formas de onda medidas en D35 (V) y D34 (I)
[ESP32 -> PC]   ADC_DATA:ALFA:<deg>;ONDA:<COMPLETA|MEDIA>;V:<v0>,...,<v63>;I:<i0>,...,<i63>
```
