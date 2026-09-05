# Firmware ESP32 - Control de Disparo y Comunicaciones en Red

Este directorio contiene el firmware embebido para el microcontrolador **ESP32** (SoC dual-core Xtensa LX6 a 240 MHz), diseñado para el control de fase en tiempo real de convertidores estáticos de potencia (rectificadores controlados mediante tiristores/SCRs o TRIACs), sincronización precisa con la red eléctrica y comunicaciones inalámbricas.

---

## 1. Arquitectura Dual-Core (FreeRTOS)

El firmware desacopla estrictamente las operaciones de tiempo crítico de las tareas de red y servicios auxiliares aprovechando los dos núcleos del ESP32:

```
                  ┌─────────────────────────────────────────────────────────┐
                  │                    ESP32 SoC (240 MHz)                  │
                  └─────────────────────────────────────────────────────────┘
                                   │                       │
                 ┌─────────────────┴─────────┐   ┌─────────┴─────────────────┐
                 │          CORE 1           │   │          CORE 0           │
                 │   (Tiempo Real Estricto)  │   │   (Comunicaciones / Red)  │
                 ├───────────────────────────┤   ├───────────────────────────┤
  PC817 (ZC) ───►│ • ISR Cruce por Cero      │   │ • Conexión Wi-Fi (STA)    │
  (GPIO 18)      │   (zero_cross_isr en IRAM)│   │ • Servidor UDP (Port 8888)│
                 │ • Hardware Timer a 1 MHz  │   │ • Servicio ArduinoOTA     │
                 │   (timer_isr en IRAM)     │   │ • Parseo de "ALFA:xx.x"   │
                 │ • Generación de pulsos    │   │ • Conversión a microseg.  │
  MOC3021 ◄──────│   (50 µs de compuerta)    │   │ • Sincronización atómica  │
  (GPIO 19)      └─────────────┬─────────────┘   └─────────────┬─────────────┘
                               │                               │
                               └───────────► MUX ◄─────────────┘
                                    (timerMux - Spinlock)
```

### Core 1: Control de Tiempo Real en Microsegundos
* **Detección de Cruce por Cero (`zero_cross_isr`):**
  * Vinculada a interrupción externa por flanco de bajada/subida en `PIN_ZERO_CROSS` (GPIO 18).
  * Código alojado en memoria rápida `IRAM_ATTR` para una latencia de atención inferior a $1\,\mu\text{s}$.
  * Detiene y resetea el temporizador de hardware en cada semiciclo ($10000\,\mu\text{s}$ para $50\text{ Hz}$).
* **Temporizador de Hardware (`timer_isr`):**
  * Temporizador de 64 bits a 1 MHz (resolución de $1\,\mu\text{s}$).
  * Se dispara exactamente tras cumplirse el retardo calculado para el ángulo $\alpha$.
  * Emite un pulso de compuerta de $50\,\mu\text{s}$ por `PIN_DISPARO` (GPIO 19) para cebar los tiristores o TRIAC a través de optoacoplador MOC3021.
* **Compensación de Offset Físico (`offset_hardware_us`):**
  * Compensa el retraso de propagación y conmutación de los optoacopladores de cruce por cero (PC817), fijado por defecto en $200\,\mu\text{s}$.

### Core 0: Capa de Red y Servicios (FreeRTOS `TaskComunicaciones`)
* **Conexión Wi-Fi Station (STA):** Enlaza el microcontrolador a la red inalámbrica de control.
* **Servidor UDP (Puerto 8888):**
  * Escucha asíncrona de datagramas de control enviados desde la aplicación de PC (`interfaz.py`).
  * Protocolo en texto plano: `"ALFA:xx.x"` (ej. `"ALFA:45.0"`).
  * Valida rango seguro ($0.0^\circ \le \alpha \le 180.0^\circ$) y realiza la conversión a microsegundos:
    $$t_{\mu s} = \left(\frac{\alpha}{180.0}\right) \times 10000\,\mu s - \text{offset}$$
* **Actualizaciones Inalámbricas (ArduinoOTA):**
  * Permite flashear nuevas versiones de firmware a través de Wi-Fi sin necesidad de desconectar el ESP32 del circuito de potencia.
* **Sincronización Inter-Core Segura (`portMUX_TYPE`):**
  * Acceso atómico a las variables globales `angulo_disparo_deg` y `angulo_disparo_us` mediante spinlocks (`portENTER_CRITICAL` / `portENTER_CRITICAL_ISR`), eliminando condiciones de carrera (*race conditions*) entre el hilo de red y la rutina de interrupción de hardware.

---

## 2. Asignación de Pines (Pinout) y Hardware

| Función | Pin ESP32 | Tipo | Dispositivo Conectado | Descripción |
| :--- | :---: | :---: | :--- | :--- |
| **Cruce por Cero (ZC)** | `GPIO 18` | Entrada Digital | Detector ZC (PC817 / 4N25) | Pulso en cada cruce por cero de la red ($50\text{ Hz}$). |
| **Disparo de Compuerta** | `GPIO 19` | Salida Digital | Driver Opto-TRIAC (MOC3021) | Pulso de $50\,\mu\text{s}$ hacia la compuerta de tiristores/TRIAC. |
| **Monitor Serie (UART)** | `TX0 / RX0` | Bidireccional | PC / Convertidor USB-Serie | Telemetría a 115200 baudios y comandos manuales de prueba. |

---

## 3. Protocolo de Comunicación UDP

El ESP32 escucha en el puerto UDP asignado (`8888`). Los mensajes recibidos deben respetar el siguiente formato:

```
ALFA:<grados>
```

**Ejemplos:**
* `ALFA:0.0` $\to$ Conducción máxima (retardo mínimo).
* `ALFA:90.0` $\to$ Disparo a mitad de semiciclo ($5000\,\mu\text{s} - \text{offset}$).
* `ALFA:150.0` $\to$ Conducción reducida.

Cualquier comando fuera del intervalo $[0.0, 180.0]$ es descartado automáticamente para prevenir condiciones inseguras en los semiconductores de potencia.

---

## 4. Configuración y Compilación

### Requisitos
* Entorno de desarrollo: **PlatformIO** (VS Code) o **Arduino IDE** con soporte para placas ESP32 instalado.
* Tarjeta seleccionada: `ESP32 Dev Module` (o compatible).

### Pasos de Configuración
1. Abrir `main.cpp`.
2. Modificar las credenciales Wi-Fi:
   ```cpp
   const char* WIFI_SSID     = "NOMBRE_DE_TU_RED";
   const char* WIFI_PASSWORD = "PASSWORD_DE_TU_RED";
   const uint16_t UDP_PORT   = 8888;
   ```
3. Compilar y cargar el firmware al ESP32 vía USB por primera vez.
4. Abrir el Monitor Serie a **115200 baudios** para verificar la IP obtenida:
   ```
   [Core 0] ¡Wi-Fi conectado con éxito!
   [Core 0] Dirección IP asignada: 192.168.1.50
   [Core 0] Servidor UDP a la escucha en el puerto 8888.
   ```
5. En la aplicación de PC ([pc_software/interfaz.py](../pc_software/interfaz.py)), ingresar dicha dirección IP para habilitar el control remoto inalámbrico.
