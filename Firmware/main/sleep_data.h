#ifndef SLEEP_DATA_H
#define SLEEP_DATA_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// --- JSON buffer size ---
#define JSON_BUF_SIZE 256

// --- Shared data struct ---
typedef struct
{
    uint8_t  presence;
    uint8_t  heart_rate;        // 0xFF = no presence, 0 = acquiring
    uint8_t  breathe_rate;      // 0xFF = no presence, 0 = acquiring
    uint8_t  apnea_events;      // 0xFF = no presence
    uint8_t  sleep_disturbance; // 0xFF = no presence
    bool     vitals_locked;
    float    temperature_c;
    char     temp_zone[16];
} SleepData_t;

// --- Globals shared across all files ---
extern SleepData_t       g_sleep_data;
extern SemaphoreHandle_t g_data_mutex;

// --- Helper ---
const char *classify_temp_zone(float temp_c);

#endif // SLEEP_DATA_H