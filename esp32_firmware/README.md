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
  * Vinculada a interrupción externa por flanco ascendente en `PIN_ZERO_CROSS` (GPIO 18).
  * Código alojado en memoria rápida `IRAM_ATTR` para una latencia de atención inferior a $1\,\mu\text{s}$.
  * **Filtro Antirrebote / Ruido Industrial:** Descarta activamente cualquier interrupción que ocurra antes de $7000\,\mu\text{s}$ desde el último cruce válido (en red de 50 Hz el semiciclo dura $10000\,\mu\text{s}$), evitando que los transitorios de línea desfasen el temporizador.
  * Detiene y resetea el temporizador de hardware en cada semiciclo.
* **Temporizador de Hardware (`timer_isr`):**
  * Temporizador de 64 bits a 1 MHz (resolución de $1\,\mu\text{s}$).
  * Se dispara exactamente tras cumplirse el retardo calculado para el ángulo $\alpha$.
  * Emite un pulso de compuerta de $50\,\mu\text{s}$ por `PIN_DISPARO` (GPIO 19) para cebar los tiristores o TRIAC a través de optoacoplador MOC3021.
* **Compensación de Offset Físico Monótona (`offset_hardware_us`):**
  * Compensa el retardo de conmutación del optoacoplador PC817 ($200\,\mu\text{s}$ por defecto).
  * Si el tiempo base es menor o igual al offset ($\alpha \le 3.6^\circ$), fija de forma segura `us_final = 10` $\mu\text{s}$, garantizando una respuesta estrictamente continua y monótona sin saltos bruscos de fase.

### Core 0: Capa de Red y Servicios (FreeRTOS `TaskComunicaciones`)
* **Conexión Wi-Fi Station (STA) con Timeout y Resiliencia:**
  * Implementa un timeout de conexión de **15 segundos** para no bloquear el microcontrolador si no hay red disponible (permitiendo operación local vía USB).
  * Incluye bucle de **reconexión automática** periódica cada 10 segundos ante pérdidas de señal Wi-Fi.
* **Servidor UDP Bidireccional (Puerto 8888):**
  * Escucha asíncrona de datagramas de control enviados desde la aplicación de PC (`interfaz.py`).
  * Protocolo en texto plano: `"ALFA:xx.x"` (ej. `"ALFA:45.0"`).
  * Valida rango seguro ($0.0^\circ \le \alpha \le 180.0^\circ$) e invoca atómicamente a `set_angulo_disparo_deg(alfa)`.
  * **Confirmación Inmediata (ACK):** Responde al remitente con `"ACK:ALFA:xx.x\n"` (o `"NACK:OUT_OF_RANGE:xx.x\n"` si está fuera de rango) permitiendo a la PC certificar la conexión en tiempo real.
* **Actualizaciones Inalámbricas (ArduinoOTA):**
  * Permite flashear nuevas versiones de firmware a través de Wi-Fi sin necesidad de desconectar el ESP32 del circuito de potencia.
* **Sincronización Inter-Core Segura (`portMUX_TYPE`):**
  * Acceso atómico a las variables globales `angulo_disparo_deg` y `angulo_disparo_us` mediante spinlocks (`portENTER_CRITICAL` / `portENTER_CRITICAL_ISR`), eliminando condiciones de carrera (*race conditions*) entre el hilo de red y la rutina de interrupción de hardware.

---

## 2. Asignación de Pines (Pinout) y Hardware

| Función | Pin ESP32 | Tipo | Dispositivo Conectado | Descripción |
| :--- | :---: | :---: | :--- | :--- |
| **Cruce por Cero (ZC)** | `GPIO 18` | Entrada Digital | Detector ZC (PC817 / 4N25) | Pulso en cada cruce por cero de la red ($50\text{ Hz}$) con filtro antirrebote de $7000\,\mu\text{s}$. |
| **Disparo de Compuerta** | `GPIO 19` | Salida Digital | Driver Opto-TRIAC (MOC3021) | Pulso de $50\,\mu\text{s}$ hacia la compuerta de tiristores/TRIAC. |
| **Monitor Serie (UART)** | `TX0 / RX0` | Bidireccional | PC / Convertidor USB-Serie | Telemetría a 115200 baudios y comandos manuales de prueba. |

---

## 3. Protocolo de Comunicación UDP (Handshake con ACK)

El ESP32 escucha en el puerto UDP asignado (`8888`). Los mensajes recibidos y respuestas respetan el siguiente formato:

```
[PC -> ESP32]   ALFA:<grados>
[ESP32 -> PC]   ACK:ALFA:<grados>   (Si está dentro de 0.0° a 180.0°)
[ESP32 -> PC]   NACK:OUT_OF_RANGE:<grados> (Si está fuera de rango)
```

**Ejemplos de Comando y Respuesta:**
* Envío: `ALFA:0.0` $\to$ Respuesta: `ACK:ALFA:0.0` (Conducción máxima, disparo seguro inmediato).
* Envío: `ALFA:90.0` $\to$ Respuesta: `ACK:ALFA:90.0` (Disparo a mitad de semiciclo).
* Envío: `ALFA:195.0` $\to$ Respuesta: `NACK:OUT_OF_RANGE:195.0` (Rechazado, ángulo descartado).

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
