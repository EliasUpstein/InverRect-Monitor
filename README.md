# Analizador de Potencia - Electrónica de Potencia

## Descripción
Este es un sistema de adquisición y análisis de parámetros eléctricos (P, S, Q, D, THD) diseñado para inversores y rectificadores controlados. Destaca por utilizar procesamiento de señales mediante correlación estricta de la Serie de Fourier (sin dependencias de FFT), lo que le permite operar de forma ligera y en tiempo real.

## Arquitectura
El software utiliza un patrón MVC (Modelo-Vista-Controlador):
- **Modelo:** Motor matemático puro en NumPy (`calculos.py`).
- **Vista y Controlador:** Interfaz gráfica interactiva y visualización implementadas con Tkinter y Matplotlib (`interfaz.py` y `main.py`).

El sistema está arquitectónicamente preparado para acoplarse a un microcontrolador (como un ESP32) para recibir muestras por ADC, aislando los cálculos complejos de la capa de visualización.

## Características Principales
* **Cálculo de Potencias Avanzadas:** Potencia Activa (P), Aparente (S), Reactiva Fundamental (Q) y Potencia de Distorsión (D).
* **Análisis de THD Robusto:** Incluye prevención de desbordamiento por ruido de punto flotante.
* **Simulación de Cargas Complejas:** Soporte para cargas resistivas y reconstrucción para impedancias complejas (RL/RC).
* **Visualización Precisa de Señales:** Muestra voltaje, corriente escalada y líneas de corte de ángulo de disparo (α) abarcando 2 ciclos completos (RMS y AVG).
* **Modo de Actualización en Tiempo Real:** Bucle recursivo ajustable para integración y monitoreo continuo.

## Instalación y Uso

1. **Clonar el repositorio:**
   ```bash
   git clone <TU_URL_REMOTO>
   cd Proyecto2C
   ```

2. **Crear y activar un entorno virtual (Recomendado):**
   ```bash
   python -m venv venv
   # En Windows:
   venv\Scripts\activate
   # En Linux/Mac:
   source venv/bin/activate
   ```

3. **Instalar dependencias:**
   ```bash
   pip install numpy matplotlib
   ```

4. **Ejecutar la aplicación:**
   ```bash
   python main.py
   ```
