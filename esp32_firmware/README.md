# Firmware ESP32 - Control de Disparo y Comunicaciones

Este directorio contiene el firmware embebido para el microcontrolador ESP32, encargado del control en tiempo real del ángulo de disparo ($\alpha$) para rectificadores/tiristores y de la adquisición de datos de instrumentación.

## Arquitectura Dual-Core (FreeRTOS)
El firmware aprovecha la arquitectura de doble núcleo (dual-core) del ESP32 para separar las tareas de tiempo crítico de las operaciones de red:

- **Core 1 (Control en Tiempo Real por Interrupciones):**
  - **Detección de Cruce por Cero (`zero_cross_isr`):** Captura el paso por cero de la red de CA mediante optoacoplador (ej. PC817) mediante interrupciones externas IRAM.
  - **Temporizador de Hardware (`timer_isr`):** Temporizador de precisión a 1 MHz (resolución de 1 $\mu s$) que gestiona el retardo exacto correspondiente al ángulo de disparo $\alpha$ con compensación de offset de hardware (`offset_hardware_us`), generando pulsos breves en la compuerta del TRIAC/Tiristor.

- **Core 0 (Comunicaciones y Telemetría):**
  - **Tarea FreeRTOS (`TaskComunicaciones`):** Ejecución independiente asignada al Core 0 para el muestreo de canales ADC y la transmisión asíncrona de telemetría vía Wi-Fi/Sockets hacia la aplicación de PC, sin interferir en la sincronización de fase.

## Estructura
- `main.cpp`: Código fuente principal implementado bajo framework Arduino y FreeRTOS.
