#include <Arduino.h>

/**
 * ============================================================================
 * Proyecto: InverRect-Monitor - Control de Rectificador con ESP32
 * Módulo: esp32_firmware/main.cpp
 * Arquitectura: Dual-Core (FreeRTOS) + Temporizadores de Hardware
 * 
 * Core 1: Control de disparo en tiempo real (Interrupciones y Hardware Timer)
 * Core 0: Tarea de comunicaciones y telemetría (Simulación ADC / Wi-Fi)
 * ============================================================================
 */

// ==========================================
// 1. DEFINICIONES DE HARDWARE Y PINES
// ==========================================
#define PIN_ZERO_CROSS 18  // Entrada digital: Cruce por cero (detector optoacoplado PC817)
#define PIN_DISPARO    19  // Salida digital: Disparo de compuerta (MOC3021 / TRIAC / Tiristor)

// Frecuencia de la red eléctrica de corriente alterna (50 Hz en Argentina / Europa, o 60 Hz)
#define FRECUENCIA_RED_HZ 50.0f 

// Duración de un semiciclo (180°) en microsegundos: T_semi = 1 / (2 * f) * 1e6 = 10000 us a 50 Hz
#define SEMIPERIODO_US    (1000000.0f / (2.0f * FRECUENCIA_RED_HZ))

// ==========================================
// 2. VARIABLES GLOBALES VOLÁTILES
// ==========================================
// Variables de control de fase modificables dinámicamente
volatile float angulo_disparo_deg = 90.0f;  // Ángulo de disparo alfa en grados sexagesimales (0.0° a 180.0°)
volatile uint32_t angulo_disparo_us = 5000; // Retardo alfa equivalente en microsegundos (calculado automáticamente)
volatile uint32_t offset_hardware_us = 200; // Compensación de retardo físico del PC817 (200 us por defecto)

// Puntero al temporizador de hardware del ESP32
hw_timer_t * timer = NULL;

// ==========================================
// 3. FUNCIONES DE CONVERSIÓN Y CONTROL
// ==========================================

/**
 * Convierte un ángulo en grados sexagesimales (0.0° - 180.0°) a tiempo en microsegundos (us).
 * Fórmula: t_us = (alfa_deg / 180.0) * SEMIPERIODO_US
 */
inline uint32_t grados_a_microsegundos(float grados) {
    if (grados < 0.0f) grados = 0.0f;
    if (grados > 180.0f) grados = 180.0f;
    return (uint32_t)((grados / 180.0f) * SEMIPERIODO_US);
}

/**
 * Actualiza el ángulo de disparo a partir de un valor en grados sexagesimales.
 * Realiza la conversión fuera de la ISR para optimizar el rendimiento del Core 1.
 */
void set_angulo_disparo_deg(float grados) {
    if (grados < 0.0f) grados = 0.0f;
    if (grados > 180.0f) grados = 180.0f;

    angulo_disparo_deg = grados;
    angulo_disparo_us = grados_a_microsegundos(grados);

    Serial.print("[Control] Nuevo ángulo fijado: ");
    Serial.print(angulo_disparo_deg, 1);
    Serial.print("° -> Retardo timer: ");
    Serial.print(angulo_disparo_us);
    Serial.println(" us");
}

// ==========================================
// 4. RUTINAS DE INTERRUPCIÓN (CORE 1 - IRAM)
// ==========================================

/**
 * ISR: Cruce por Cero (Zero Cross Detection)
 * Se ejecuta inmediatamente tras cada cruce por cero de la tensión de red.
 * Detiene el timer anterior, calcula el retardo y lo reinicia.
 */
void IRAM_ATTR zero_cross_isr() {
    // 1. Detener el temporizador si estaba corriendo y reiniciar contador a cero
    timerStop(timer);
    timerWrite(timer, 0);

    // 2. Calcular el retardo efectivo de disparo
    // El optoacoplador PC817 introduce un tiempo de subida/bajada (offset).
    // Dependiendo del circuito detector:
    // - Si detecta con retraso: se compensa restando (angulo_disparo_us - offset_hardware_us).
    // - Si el umbral ocurre antes del cruce real: se compensa sumando (angulo_disparo_us + offset_hardware_us).
    uint32_t tiempo_alarma;
    if (angulo_disparo_us > offset_hardware_us) {
        tiempo_alarma = angulo_disparo_us - offset_hardware_us;
    } else {
        tiempo_alarma = angulo_disparo_us + offset_hardware_us;
    }

    // Protección mínima para evitar valores nulos
    if (tiempo_alarma < 10) {
        tiempo_alarma = 10;
    }

    // 3. Configurar alarma y reiniciar temporizador en modo disparo único (autoreload = false)
    timerAlarmWrite(timer, tiempo_alarma, false);
    timerAlarmEnable(timer);
    timerStart(timer);
}

/**
 * ISR: Temporizador de Hardware
 * Se dispara cuando se cumple el tiempo correspondiente al ángulo alfa.
 * Emite un pulso corto de compuerta para activar el tiristor/TRIAC.
 */
void IRAM_ATTR timer_isr() {
    // Generar pulso de disparo (Gate trigger)
    digitalWrite(PIN_DISPARO, HIGH);
    
    // Retardo breve para asegurar enganche del tiristor (50 microsegundos en IRAM)
    ets_delay_us(50);
    
    digitalWrite(PIN_DISPARO, LOW);
}

// ==========================================
// 4. TAREAS DE FREERTOS (CORE 0)
// ==========================================

/**
 * Tarea de Comunicaciones y Telemetría
 * Ejecuta en el Core 0 de manera desacoplada para no afectar el timing crítico del Core 1.
 */
void TaskComunicaciones(void *pvParameters) {
    (void) pvParameters;

    // Inicialización de comunicaciones (Wi-Fi, Sockets, etc. a implementar)
    Serial.println("[Core 0] Tarea de comunicaciones iniciada en segundo plano.");

    for (;;) {
        // Simulación: Adquisición de muestras de ADC y envío de datos de telemetría a la PC
        // En implementaciones futuras:
        // - Leer canales ADC de tensión y corriente
        // - Empaquetar y enviar vía Wi-Fi / UDP / WebSockets
        
        // Retardo no bloqueante para ceder tiempo a la pila Wi-Fi de FreeRTOS
        vTaskDelay(pdMS_TO_TICKS(100)); // Periodo de 100 ms (10 Hz de refresco)
    }
}

// ==========================================
// 5. SETUP Y CONFIGURACIÓN PRINCIPAL
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n--- InverRect-Monitor: Inicializando Firmware ESP32 ---");

    // 1. Configuración de Pines GPIO
    pinMode(PIN_ZERO_CROSS, INPUT_PULLUP);
    pinMode(PIN_DISPARO, OUTPUT);
    digitalWrite(PIN_DISPARO, LOW);

    // 2. Inicialización del ángulo de disparo en grados sexagesimales (conversión a us)
    set_angulo_disparo_deg(angulo_disparo_deg);

    // 3. Inicialización del Temporizador de Hardware
    // Prescaler = 80 -> Con reloj base de 80 MHz: 80 MHz / 80 = 1 MHz (1 tick = 1 us)
    // Parámetros: timerBegin(numero_timer, prescaler, cuenta_arriba)
    timer = timerBegin(0, 80, true);

    // 4. Vincular interrupción del timer a timer_isr
    timerAttachInterrupt(timer, &timer_isr, true);

    // Configuración inicial de alarma en modo single-shot (autoreload = false)
    timerAlarmWrite(timer, angulo_disparo_us, false);
    timerAlarmEnable(timer);

    // 5. Adjuntar interrupción externa de Cruce por Cero (flanco ascendente)
    attachInterrupt(digitalPinToInterrupt(PIN_ZERO_CROSS), zero_cross_isr, RISING);

    // 6. Creación de la Tarea FreeRTOS asignada al Core 0
    xTaskCreatePinnedToCore(
        TaskComunicaciones,      /* Función que implementa la tarea */
        "TaskComunicaciones",    /* Nombre descriptivo de la tarea */
        4096,                    /* Tamaño de stack asignado (en bytes/words) */
        NULL,                    /* Parámetro de entrada a la tarea */
        1,                       /* Prioridad de la tarea (1 = baja/media) */
        NULL,                    /* Handle de la tarea */
        0                        /* Núcleo de ejecución: Core 0 */
    );

    Serial.println("[Core 1] Interrupciones de Cruce por Cero y Temporizador activas.");
    Serial.println("--- Sistema listo para control de fase (Envía grados por Serial para cambiar) ---");
}

// ==========================================
// 6. LOOP PRINCIPAL (CORE 1)
// ==========================================
void loop() {
    // Permite ajustar el ángulo de disparo dinámicamente vía Monitor Serial (ej. enviar '45.0')
    if (Serial.available() > 0) {
        float nuevo_angulo = Serial.parseFloat();
        if (nuevo_angulo >= 0.0f && nuevo_angulo <= 180.0f) {
            set_angulo_disparo_deg(nuevo_angulo);
        }
        // Limpiar caracteres remanentes en el buffer serial
        while (Serial.available() > 0) {
            Serial.read();
        }
    }

    // El Core 1 cede tiempo para no saturar la CPU
    vTaskDelay(pdMS_TO_TICKS(100));
}
