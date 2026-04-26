#ifndef UART_C1001_SENSOR_H
#define UART_C1001_SENSOR_H

#include <stdint.h>
#include "driver/uart.h"
#include "driver/gpio.h"

// --- UART Config ---
#define SENSOR_UART_PORT     UART_NUM_2
#define SENSOR_TX_PIN        GPIO_NUM_4
#define SENSOR_RX_PIN        GPIO_NUM_5
#define SENSOR_BAUD_RATE     115200
#define SENSOR_RX_BUF_SIZE   1024
#define SENSOR_TX_BUF_SIZE   1024

// --- Sensor Data ---
typedef struct {
    uint8_t  presence;      // 0 = no one, 1 = present
    uint8_t  heart_rate;    // bpm, 0xFF = not locked
    uint8_t  breathe_rate;  // bpm
} C1001_Sensor_Data_t;

// --- Public API ---
esp_err_t C1001_init(void);
esp_err_t C1001_get_data(C1001_Sensor_Data_t *out);

#endif // UART_C1001_SENSOR_H