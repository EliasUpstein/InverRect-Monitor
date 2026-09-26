# Firmware ESP32 - Control de Disparo, ADC y Comunicaciones en Red

Este directorio contiene el firmware embebido para el microcontrolador **ESP32** (SoC dual-core Xtensa LX6 a 240 MHz), diseñado para el control de fase en tiempo real de convertidores estáticos de potencia (rectificadores controlados mediante tiristores/SCRs o TRIACs). 

Esta versión combina y optimiza el control analógico por potenciómetro y temporización ZCD con la arquitectura dual-core FreeRTOS y el protocolo de control remoto inalámbrico por UDP con la consola SCADA de PC.

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
  (GPIO 4)       │   (isrCrucePorCero - IRAM)│   │ • Servidor UDP (Port 8888)│
                 │ • Medición Dinámica ZCD   │   │ • Servicio ArduinoOTA     │
  Potenciómetro ─►│   (periodoCicloCompleto)  │   │   con suspensión segura   │
  (GPIO 36 ADC)  │ • Lectura ADC Potencióm.  │   │   de potencia en onStart  │
                 │   (Mapeo ADC 0-4095->5-175°)│   │ • Parseo de "ALFA:xx.x" y │
  MOC3021 ◄──────│ • Disparo de Compuerta    │   │   "ALFA:ADC" / "MODO:ADC" │
  (GPIO 16)      │   (Pulso de 50 µs)        │   │ • Sincronización atómica  │
                 └─────────────┬─────────────┘   └─────────────┬─────────────┘
                               │                               │
                               └───────────► MUX ◄─────────────┘
                                    (timerMux - Spinlock)
```

### Core 1: Control de Tiempo Real y Potencia (Procesamiento Físico)
* **Detección de Cruce por Cero (`isrCrucePorCero`):**
  * Vinculada a interrupción por flanco descendente (`FALLING`) en **GPIO 4**.
  * Medición dinámica en microsegundos del periodo de ciclo (`periodoCicloCompleto = micros() - tiempoUltimoCruce`), permitiendo adaptarse automáticamente a redes de 50 Hz ($20000\,\mu\text{s}$) o 60 Hz ($16667\,\mu\text{s}$).
  * **Filtro Antirrebote Industrial:** Descarta ruidos y armónicas espurias antes de $12000\,\mu\text{s}$ (`FILTRO_DEBOUNCE_ZCD_US`).
* **Modo Potenciómetro Analógico (ADC - GPIO 36):**
  * Configuración ADC1 a 12 bits ($0-4095$).
  * Mapeo dinámico y continuo del ángulo $\alpha$ entre $5.0^\circ$ y $175.0^\circ$ en tiempo real cuando `!cruceDetectado`, garantizando que el disparo nunca colisione con el cruce por cero.
* **Generación de Pulso de Compuerta (GPIO 16):**
  * Generación de pulso limpio de $50\,\mu\text{s}$ para el opto-TRIAC (MOC3021) / Tiristores.
  * Retardo calculado con compensación física de optoacoplador (`ADELANTO_COMPENSACION_US = 100` $\mu\text{s}$).

### Core 0: Capa de Red y Servicios (FreeRTOS `TaskComunicaciones`)
* **Servicio ArduinoOTA Seguro:**
  * Al iniciar un flasheo inalámbrico (`onStart`), se desvincula la interrupción ZCD (`detachInterrupt`) y se fuerza `GPIO 16` a `LOW`, suspendiendo la etapa de potencia para evitar disparos erráticos o destrucciones de semiconductores.
* **Servidor UDP Bidireccional (Puerto 8888):**
  * Permite conmutar dinámicamente entre **Control Manual Fijo (`ALFA:60.0`)** y **Control por Potenciómetro Analógico (`ALFA:ADC`)**.
  * Emite acuses de recibo (`ACK:ALFA:xx.x` / `ACK:ALFA:ADC`) a la interfaz gráfica SCADA.

---

## 2. Asignación de Pines (Pinout de Hardware)

| Función | Pin ESP32 | Tipo | Dispositivo Conectado | Descripción |
| :--- | :---: | :---: | :--- | :--- |
| **Cruce por Cero (ZC)** | `GPIO 4` | Entrada Digital | Optoacoplador (4N25 / PC817) | Detección de cruce por cero (`FALLING`) con filtro antirrebote de 12 ms. |
| **Disparo Compuerta** | `GPIO 16` | Salida Digital | Driver Opto-TRIAC (MOC3021) | Pulso de $50\,\mu\text{s}$ hacia compuerta de tiristores / TRIAC. |
| **Potenciómetro Analógico** | `GPIO 36` | Entrada Analógica | Potenciómetro (ADC1 Channel 0 / VP) | Variación analógica local del ángulo $\alpha$ ($5^\circ$ a $175^\circ$). |
| **Monitor Serie (UART)** | `TX0 / RX0` | Bidireccional | PC / Convertidor USB-Serie | Telemetría a 115200 baudios y comandos de prueba (`ADC` / número). |

---

## 3. Protocolo de Comunicación UDP (Handshake Híbrido)

El ESP32 escucha en el puerto UDP `8888`:

```
[PC -> ESP32]   ALFA:ADC            --> Activa control analógico por potenciómetro (GPIO 36)
[ESP32 -> PC]   ACK:ALFA:ADC:<deg>  --> Confirma activación de modo ADC y ángulo actual

[PC -> ESP32]   ALFA:45.0           --> Fija ángulo manual en 45.0° (Desactiva modo ADC)
[ESP32 -> PC]   ACK:ALFA:45.0       --> Confirma ángulo fijado
```

---

## 4. Verificación y Ejecución de Pruebas

Para verificar el funcionamiento del firmware:
1. Abrir `esp32_firmware/esp32_firmware.ino` en Arduino IDE o PlatformIO.
2. Modificar `WIFI_SSID` y `WIFI_PASSWORD` con las credenciales de la red.
3. Flashear la placa ESP32 por primera vez mediante USB / Serial.
4. Ejecutar la consola SCADA de PC: `python pc_software/main.py`.
5. Seleccionar la opción **`11. Muestras ADC (Simuladas)`** en el desplegable y presionar **"Enviar Ángulo (α) al Hardware"**: la consola transmitirá `ALFA:ADC` y el ESP32 responderá pasando al control en tiempo real mediante el potenciómetro analógico en GPIO 36.
