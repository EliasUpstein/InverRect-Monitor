# Software de PC - Analizador de Potencia y Panel SCADA

Este directorio contiene la aplicación de escritorio del proyecto **InverRect-Monitor**, desarrollada en Python. Combina un entorno de instrumentación y supervisión gráfica SCADA con un motor matemático de cálculo numérico para el análisis en profundidad de calidad de energía y convertidores estáticos de potencia.

---

## 1. Arquitectura de Software (Patrón MVC)

El software está estrictamente organizado bajo el patrón de arquitectura **Modelo-Vista-Controlador (MVC)** para garantizar modularidad, mantenibilidad y separación de responsabilidades:

```
pc_software/
├── calculos.py   # [MODELO]      Motor matemático puro (NumPy, Fourier, IEEE 1459)
├── interfaz.py   # [VISTA]       Interfaz gráfica SCADA (Tkinter + Matplotlib + Sockets UDP)
├── main.py       # [CONTROLADOR] Punto de entrada y gestión del ciclo de vida
└── README.md     # Documentación técnica del software de PC
```

### Modelo (`calculos.py` - Clase `AnalizadorDePotencia`)
* **Procesamiento de Señales:** Genera y sintetiza las formas de onda de tensión en el dominio discreto temporal para 12 topologías distintas.
* **Correlación Trigonométrica de Fourier:** Descompone las señales en sus coeficientes armónicos de Fourier ($a_n, b_n$) mediante integración numérica directa sin depender de la Transformada Rápida de Fourier (FFT), lo que garantiza máxima precisión incluso con un número reducido de muestras por ciclo.
* **Modelado de Impedancias Complejas:** Calcula la corriente en régimen estacionario ante cargas resistivas-inductivas ($R$-$L$) o resistivas-capacitivas ($R$-$C$), adaptando el desfase y la atenuación para cada armónica individual ($X_n = nX$ o $X_n = X/n$).
* **Calidad de Energía (IEEE 1459):** Computa tensiones y corrientes eficaces ($V_{\text{rms}}, I_{\text{rms}}$), potencia activa ($P$), aparente ($S$), reactiva fundamental ($Q$), distorsión armónica ($D$), factor de potencia ($FP$) y distorsión armónica total ($\text{THD}_V, \text{THD}_I$) mediante umbrales dinámicos adaptativos (`max(1e-6, 1e-4 * rms)`) que permiten medir corrientes bajas en miliamperios sin discriminación artificial de armónicas.

### Vista (`interfaz.py` - Clase `InterfazGrafica`)
* **Diseño SCADA de Alta Legibilidad:** Estilo moderno `clam` adaptado para monitores industriales y de laboratorio (optimizado para resoluciones desde $1366 \times 768$ hasta 1080p con escalado).
* **Bloques Funcionales:**
  1. **Parámetros de Entrada:** Configuración de topología de señal, tensión pico ($V_p$), ángulo de disparo ($\alpha$ en grados o radianes), carga ($R$ y $X$), armónicas a calcular y muestras por ciclo. Incluye verificación estricta del **Teorema de Nyquist** ($N_{\text{arm}} < N_{\text{muestras}}/2$) para impedir aliasing espectral.
  2. **Actualización en Tiempo Real:** Bucle configurable con temporizador cancelable activamente (`self.timer_id`) al desactivar el checkbox o cerrar la aplicación, evitando acumulación de procesos concurrentes.
  3. **Control de Hardware (ESP32):** Campo de entrada interactivo para configurar la IP del ESP32 dinámicamente (`self.ent_esp32_ip`), visualizador de estado con código de color (verde: conectado, rojo: timeout/error, azul: enviando) y botón de despacho.
  4. **Resultados Numéricos:** Panel de métricas en fuente monoespaciada (`Consolas 11`) con visualización completa sin recortes.
* **Gráficos Interactivos (Matplotlib):**
  * **Subplot 1 (Tiempo):** Reconstrucción de ondas de tensión y corriente en 2 ciclos completos, con marcado de líneas de corte de fase $\alpha$.
  * **Subplot 2 (Frecuencia):** Espectro de barras comparativo de las componentes armónicas RMS de tensión y corriente.
  * **Estabilidad Visual:** Configuración de márgenes fijos mediante `subplots_adjust()` en la inicialización, evitando saltos de escala y recálculos innecesarios de `tight_layout()`.
  * **Cursores y Anotaciones por Clic:** Inspección dinámica de amplitud, ángulo y armónicas al hacer clic en los gráficos.

### Controlador (`main.py`)
* Inicializa la ventana `Tk`, conecta el protocolo de cierre seguro (`WM_DELETE_WINDOW`) cancelando cualquier temporizador de refresco activo y ejecuta el bucle de eventos principal `mainloop()`.

---

## 2. Catálogo de Topologías y Señales Soportadas

El selector de señales implementa 12 tipos de formas de onda estandarizadas:

| Índice | Nombre en UI | Descripción y Comportamiento |
| :---: | :--- | :--- |
| **0** | `0. Senoidal` | Onda senoidal pura $V_p \sin(\theta)$ de referencia. |
| **1** | `1. Cuadrada` | Onda cuadrada simétrica bipolar con transiciones instantáneas. |
| **2** | `2. Cuasi-cuadrada` | Onda con escalón y muescas regulables mediante el ángulo $\alpha$. |
| **3** | `3. RMMO` | Rectificador Monofásico de Media Onda no controlado (diodo). |
| **4** | `4. RMMOC` | Rectificador Monofásico de Media Onda Controlado (tiristor con disparo en $\alpha$). |
| **5** | `5. RMOC` | Rectificador Monofásico de Onda Completa no controlado (puente de diodos). |
| **6** | `6. RMOCC` | Rectificador Monofásico de Onda Completa Controlado (puente completo de tiristores). |
| **7** | `7. RTMO` | Rectificador Trifásico de Media Onda no controlado (conmutación natural en $\pi/6$). |
| **8** | `8. RTMOC` | Rectificador Trifásico de Media Onda Controlado (retardo $\alpha$ desde $\pi/6$). |
| **9** | `9. RTOC` | Rectificador Trifásico de Onda Completa / Puente Graetz de 6 pulsos. |
| **10** | `10. RTOCC` | Rectificador Trifásico de Onda Completa Controlado (6 tiristores con control $\alpha$). |
| **11** | `11. Muestras ADC (Simuladas)` | Señal senoidal con inyección de ruido gaussiano ($\sigma = 0.1 V_p$), simulando telemetría analógica real. Permite editar el ángulo $\alpha$ para enviar comandos de hardware sin alterar la señal simulada. |

---

## 3. Capa de Comunicación UDP y Verificación de Conexión (Enlace con ESP32)

La aplicación implementa un cliente UDP robusto con **verificación bidireccional mediante acuse de recibo (ACK/NACK)** para asegurar que el comando realmente llegó y fue aplicado en el microcontrolador:

* **Parámetros de Red y Configuración Dinámica:**
  * Dirección IP configurable directamente desde el panel de control (ej. `192.168.1.50`).
  * Validación sintáctica previa con el módulo `ipaddress` para rechazar formatos erróneos antes de emitir tráfico de red.
  * Puerto de escucha UDP local del ESP32: `8888`.
* **Protocolo de Comunicación Bidireccional:**
  1. **Comando de la PC al ESP32:** Trama en texto plano `"ALFA:xx.x"` (ej. `"ALFA:45.0"`).
  2. **Respuesta del ESP32 a la PC:**
     * Aceptado: `"ACK:ALFA:xx.x\n"`
     * Rechazado: `"NACK:OUT_OF_RANGE:xx.x\n"`
* **Detección de Timeout y Fallos de Red:**
  * El socket opera con un timeout estricto de **1.2 segundos** (`sock.settimeout(1.2)`).
  * Si la IP es inaccesible, el ESP32 está apagado o el puerto está cerrado, se captura `socket.timeout` o `ConnectionResetError` (ICMP Port Unreachable), pasando el estado a **rojo** y alertando al usuario con un diálogo explicativo sin falsos positivos.
  * Solo al recibir la trama `ACK:ALFA:` el estado cambia a **verde** certificando la conexión real con el hardware.
* **Validación de Seguridad Industrial:**
  * Rango físico admitido: $0.0^\circ \le \alpha \le 180.0^\circ$.
  * Bloqueo local contra números no válidos, NaN e infinitos.

---

## 4. Requisitos e Instalación

### Requisitos Previos
* **Python 3.8** o superior (probado en Python 3.10, 3.11, 3.12 y 3.14).
* Sistema operativo: Windows 10/11, Linux o macOS.

### Instalación de Dependencias
Se recomienda utilizar un entorno virtual de Python:

```bash
# Crear entorno virtual
python -m venv venv

# Activar entorno virtual
# En Windows:
venv\Scripts\activate
# En Linux/macOS:
source venv/bin/activate

# Instalar librerías requeridas
pip install numpy matplotlib
```

*(Nota: `tkinter` y `socket` forman parte de la biblioteca estándar de Python).*

### Ejecución de la Aplicación
Desde la raíz del repositorio o dentro de `pc_software/`:

```bash
python pc_software/main.py
```
