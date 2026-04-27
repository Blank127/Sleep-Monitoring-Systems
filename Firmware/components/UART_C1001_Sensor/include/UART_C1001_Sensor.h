/**
 * @file UART_C1001_Sensor.h
 * @brief ESP-IDF driver for the C1001 mmWave human detection sensor.
 *
 * Communicates over UART using the C1001 binary protocol.
 * Provides presence detection, heart rate, and breathing rate in sleep mode.
 *
 * Typical usage:
 *   1. Call C1001_init() once at startup from a dedicated task
 *   2. Call C1001_get_data() in a loop to poll sensor readings
 */
#ifndef UART_C1001_SENSOR_H
#define UART_C1001_SENSOR_H

#include <stdint.h>
#include "driver/uart.h"
#include "driver/gpio.h"

// --- UART Config ---
#define SENSOR_UART_PORT    UART_NUM_2
#define SENSOR_TX_PIN       GPIO_NUM_4
#define SENSOR_RX_PIN       GPIO_NUM_5
#define SENSOR_BAUD_RATE    115200
#define SENSOR_RX_BUF_SIZE  1024
#define SENSOR_TX_BUF_SIZE  1024

// --- Sensor Data ---
typedef struct 
{
    uint8_t presence;           // 0 = no one, 1 = present
    uint8_t heart_rate;         // BPM — 0 = acquiring, 0xFF = no presence
    uint8_t breathe_rate;       // BPM — 0 = acquiring, 0xFF = no presence
    uint8_t apnea_events;       // count of apnea events, 0xFF = read error
    uint8_t sleep_disturbance;  // 0-2 = abnormal, 3 = none, 0xFF = read error
} C1001_Sensor_Data_t;

// --- Public API ---

/** @brief Initialize UART and the C1001 sensor. Blocks ~30s on first call.
 *  @return ESP_OK on success, ESP_ERR_TIMEOUT if sensor does not respond */
esp_err_t C1001_init(void);

/** @brief Read presence, heart rate, and breathing rate into out.
 *  @return ESP_OK on success, ESP_ERR_INVALID_ARG if out is NULL,
 *          ESP_ERR_TIMEOUT if a sensor query times out */
esp_err_t C1001_get_data(C1001_Sensor_Data_t *out);

#endif // UART_C1001_SENSOR_H