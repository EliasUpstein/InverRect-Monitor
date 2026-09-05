# InverRect-Monitor

**Plataforma Ciberfísica de Simulación, Control HIL y Monitoreo SCADA para Electrónica de Potencia**

![Interfaz del Analizador de Potencia](assets/captura.png)

---

## 1. Visión y Propósito del Proyecto

**InverRect-Monitor** es una plataforma integral de ingeniería que fusiona la **simulación matemática de alta precisión** con el **control y monitoreo físico en tiempo real** de convertidores estáticos de potencia (rectificadores controlados y no controlados monofásicos y trifásicos, e inversores).

En el estudio y operación de la electrónica de potencia, existe a menudo una desconexión entre:
1. **Los modelos analíticos teóricos:** cálculo de potencias bajo condiciones no senoidales, descomposición en series de Fourier, impacto de armónicas en la red y comportamiento de cargas complejas ($R$, $L$, $C$).
2. **La implementación física de control:** detección precisa del cruce por cero de la red ($50\text{ Hz} / 60\text{ Hz}$), sincronización de microsegundos para el disparo de compuertas (SCR / TRIAC) y aislamiento galvánico de alta tensión.

**InverRect-Monitor** resuelve esta brecha creando un entorno tipo **Hardware-in-the-Loop (HIL) y SCADA**:
* Permite al usuario **analizar, simular y descomponer armónicamente** 12 topologías de convertidores bajo el estándar internacional **IEEE 1459**.
* Permite actuar como **consola SCADA de control remoto**, enviando el ángulo de disparo ($\alpha$) en tiempo real vía **Wi-Fi / UDP** hacia un microcontrolador **ESP32** dedicado al control físico de potencia, garantizando sincronismo estricto e inmunidad a retardos de software.

---

## 2. Arquitectura Global del Sistema Ciberfísico

El sistema opera mediante una división estricta entre la **estación de supervisión de alto nivel (PC)** y el **nodo de control de potencia en tiempo real (ESP32)**:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      ESTACIÓN DE SUPERVISIÓN (PC)                           │
│                                                                             │
│   ┌─────────────────────┐   ┌────────────────────────┐   ┌──────────────┐   │
│   │     calculos.py     │◄─►│      interfaz.py       │◄─►│   main.py    │   │
│   │ (NumPy/IEEE 1459/SF)│   │(Tkinter/Matplotlib/UDP)│   │ (Controller) │   │
│   └─────────────────────┘   └───────────┬────────────┘   └──────────────┘   │
└─────────────────────────────────────────┼───────────────────────────────────┘
                                          │ Datagramas UDP ("ALFA:xx.x")
                                          │ Wi-Fi 802.11 (Puerto 8888)
                                          ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                   NODO DE CONTROL EN TIEMPO REAL (ESP32)                    │
│                                                                             │
│   ┌─────────────────────────────────────┐   ┌───────────────────────────┐   │
│   │               CORE 0                │   │          CORE 1           │   │
│   │   • Tarea FreeRTOS Comunicaciones   │   │   • ISR Cruce por Cero    │   │
│   │   • Servidor UDP & ArduinoOTA       │◄─►│   • Hardware Timer (1 MHz)│   │
│   │   • Sincronización atómica (MUX)    │   │   • Pulso de compuerta    │   │
│   └─────────────────────────────────────┘   └─────────────┬─────────────┘   │
└───────────────────────────────────────────────────────────┼─────────────────┘
                                                            │
                      ┌─────────────────────────────────────┴─────────────┐
                      ▼                                                   ▼
            ┌───────────────────┐                               ┌───────────────────┐
            │  ENTRADA DIGITAL  │                               │  SALIDA DIGITAL   │
            │  GPIO 18 (ZC)     │                               │  GPIO 19 (Gate)   │
            └─────────▲─────────┘                               └─────────┬─────────┘
                      │                                                   │
                      │ Pulso 50 Hz                                       │ Pulso 50 µs
            ┌─────────┴─────────┐                               ┌─────────┴─────────┐
            │ Optoacoplador ZC  │                               │ Driver Opto-TRIAC │
            │      (PC817)      │                               │     (MOC3021)     │
            └─────────▲─────────┘                               └─────────┬─────────┘
                      │                                                   │
     Red Eléctrica ───┴───────────────── Circuito de Potencia ────────────┴──► Carga
     (220V CA / 50 Hz)                   (Tiristores SCR / TRIAC)              (R-L-C)
```

---

## 3. Estructura del Monorepo y Documentación Detallada

El proyecto está organizado de forma modular. Cada subsistema cuenta con su documentación técnica exhaustiva:

```
InverRect-Monitor/
├── pc_software/             # Software de PC: Analizador matemático y panel SCADA
│   ├── calculos.py          # Modelo: Síntesis de ondas, Fourier y cálculo IEEE 1459
│   ├── interfaz.py          # Vista: Interfaz gráfica industrial Tkinter + cliente UDP
│   ├── main.py              # Controlador: Ciclo de vida de la aplicación
│   └── README.md            # 📖 Documentación técnica completa del software de PC
│
├── esp32_firmware/          # Firmware embebido para el microcontrolador ESP32
│   ├── main.cpp             # Firmware Dual-Core (FreeRTOS, Hardware Timer, Wi-Fi, UDP, OTA)
│   └── README.md            # 📖 Documentación técnica de hardware, pines y firmware
│
├── assets/                  # Diagramas y capturas de pantalla
├── .gitignore               # Filtros de Git para entornos Python y compilaciones C++
└── README.md                # 📖 Visión general del proyecto (este archivo)
```

* 👉 **[Documentación del Software de PC (`pc_software/README.md`)](pc_software/README.md):** Contiene la explicación de los cálculos matemáticos (Fourier, impedancias complejas $R$-$X$, formulación IEEE 1459), descripción de las 12 señales simuladas, guía de la interfaz gráfica y configuración del cliente UDP.
* 👉 **[Documentación del Firmware ESP32 (`esp32_firmware/README.md`)](esp32_firmware/README.md):** Contiene el diagrama de conexionado eléctrico, pinout (GPIO 18 y 19), configuración de FreeRTOS, actualización inalámbrica vía ArduinoOTA y sincronización atómica con spinlocks.

---

## 4. Características Principales

### 🔬 Análisis Matemático y Procesamiento de Señales
* **Correlación Trigonométrica de Fourier (sin FFT):** Cálculo exacto de coeficientes ($a_n, b_n$) mediante integración numérica en el dominio temporal discreto. Evita problemas de *picket-fence* y dispersión espectral (*spectral leakage*) propios de la FFT en señales no periódicas o truncadas.
* **Norma IEEE 1459:** Computa potencia activa ($P$), aparente ($S$), reactiva fundamental ($Q$), distorsión armónica ($D$), factor de potencia ($FP = P/S$) y distorsión armónica total de tensión y corriente ($\text{THD}_V, \text{THD}_I$).
* **Modelado de Impedancias Complejas:** Respuesta dinámica de corriente considerando armónica fundamental y armónicas superiores ($X_n = nX$ para cargas inductivas y $X_n = X/n$ para capacitivas).
* **Biblioteca de 12 Topologías:** Senoidal pura, cuadrada, cuasi-cuadrada, rectificadores monofásicos y trifásicos (media onda y onda completa, controlados y no controlados) y simulación de entrada analógica ADC con ruido gaussiano.

### 🖥️ Interfaz Gráfica SCADA de Alta Legibilidad
* **Diseño Industrial:** Optimizado para pantallas industriales y monitores con escalado DPI (hasta 1080p con 125% DPI scaling).
* **Visualización en Dos Subplots:**
  * Dominio del tiempo: tensión y corriente superpuestas en 2 ciclos con marcación del ángulo $\alpha$.
  * Dominio de la frecuencia: espectro de barras lado a lado de armónicas RMS de tensión y corriente.
* **Cursores Interactivos:** Inspección de valores puntuales al hacer clic sobre cualquier punto de las curvas.
* **Modo en Tiempo Real:** Bucle de simulación y refresco continuo configurable en segundos.

### ⚡ Control de Hardware Físico en Tiempo Real
* **Microsegundos de Precisión:** El ESP32 ejecuta la sincronización de cruce por cero y el temporizador de hardware en el Core 1 con código en `IRAM`, asegurando un disparo libre de jitter.
* **Enlace Inalámbrico Bidireccional:** El operador ingresa el ángulo en la PC y un cliente UDP transmite la trama `"ALFA:xx.x"` al ESP32 a través de Wi-Fi.
* **Protección de Seguridad Industrial:** Validación automática en la interfaz de usuario que limita el ángulo de disparo estrictamente al rango seguro $0.0^\circ \le \alpha \le 180.0^\circ$, bloqueando valores fuera de rango para prevenir daños en los semiconductores.
* **Flasheo Inalámbrico (OTA):** Soporte para reprogramar el firmware del ESP32 a distancia mientras se encuentra instalado en el banco de pruebas.

---

## 5. Modos de Operación

El sistema puede utilizarse en tres modalidades operativas:

1. **Modo Simulación Pura (Educativo / Diseño):**
   * Se selecciona cualquiera de las topologías de convertidor (índices 0 a 10).
   * Se ajusta la amplitud de tensión, el ángulo de disparo teórico $\alpha$ y los valores de $R$ y $X$.
   * La aplicación reconstruye las ondas analíticas y calcula inmediatamente todos los parámetros de calidad de energía y el espectro armónico.
2. **Modo Hardware-in-the-Loop (HIL) y Control Remoto:**
   * La aplicación de PC se conecta al ESP32 ingresando su dirección IP en la sección de control de hardware.
   * Al modificar $\alpha$ y presionar **"Enviar Ángulo (α) al Hardware"**, el ESP32 actualiza instantáneamente el retardo de compuerta en el circuito de tiristores real.
   * Permite contrastar la respuesta teórica calculada en la PC contra las mediciones en osciloscopio tomadas sobre el convertidor físico.
3. **Modo Instrumentación / Telemetría ADC:**
   * Seleccionando la señal `11. Muestras ADC (Simuladas)`, el sistema evalúa señales analógicas adquiridas con componentes estocásticos de ruido.
   * Permite continuar despachando órdenes de hardware al ESP32 manteniendo independiente el análisis de la señal capturada.

---

## 6. Puesta en Marcha Rápida (Quickstart)

### Paso 1: Clonar el Repositorio
```bash
git clone https://github.com/EliasUpstein/InverRect-Monitor.git
cd InverRect-Monitor
```

### Paso 2: Configurar el Entorno Python
```bash
# Crear entorno virtual
python -m venv venv

# Activar entorno virtual
# En Windows:
venv\Scripts\activate
# En Linux/macOS:
source venv/bin/activate

# Instalar dependencias
pip install numpy matplotlib
```

### Paso 3: Ejecutar la Interfaz de PC
```bash
python pc_software/main.py
```

### Paso 4 (Opcional): Cargar el Firmware al ESP32
1. Abrir la carpeta `esp32_firmware/` en PlatformIO o Arduino IDE.
2. Configurar `WIFI_SSID` y `WIFI_PASSWORD` en `esp32_firmware/main.cpp`.
3. Flashear el microcontrolador y observar la IP en el Monitor Serie (115200 bps).
4. Configurar la IP en `pc_software/interfaz.py` para habilitar el control inalámbrico.

---

## 7. Referencias Normativas y Bibliográficas
* **IEEE Std 1459-2010:** *IEEE Standard Definitions for the Measurement of Electric Power Quantities Under Sinusoidal, Nonsinusoidal, Balanced, or Unbalanced Conditions*.
* **Mohan, Undeland, Robbins:** *Power Electronics: Converters, Applications, and Design*, John Wiley & Sons.
* **Rashid, Muhammad H.:** *Power Electronics Handbook*, Academic Press.
* **Espressif Systems:** *ESP32 Technical Reference Manual & FreeRTOS Architecture*.
