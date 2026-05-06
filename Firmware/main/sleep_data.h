/**
 * @file sleep_data.h
 * @brief Shared sensor data types and globals for the Sleep Monitor firmware.
 *
 * Defines the central SleepData_t struct that is written by the sensor tasks
 * and read by the payload task. All access to g_sleep_data must be protected
 * by g_data_mutex to prevent data races between the three FreeRTOS tasks.
 *
 * Sentinel values used throughout the struct:
 *   - 0    = sensor is still acquiring a lock (heart_rate, breathe_rate only)
 *   - 0xFF = field is not applicable (no presence detected)
 *
 * This header is included by every file in the project — keep it minimal.
 */

#ifndef SLEEP_DATA_H
#define SLEEP_DATA_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────

/** @def JSON_BUF_SIZE
 *  @brief Size of the JSON serialisation buffer in bytes.
 *
 *  Must be large enough to hold the longest possible reading packet
 *  produced by build_reading_packet(). 256 bytes provides comfortable
 *  headroom — the longest packet is approximately 120 bytes.
 */
#define JSON_BUF_SIZE 256

// ─────────────────────────────────────────────────────────────────────────────
// Shared data structure
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Sensor readings shared between all three FreeRTOS tasks.
 *
 * Written by c1001_task (presence, vitals) and ds18b20_task (temperature).
 * Read and transmitted by payload_task every 10 seconds.
 *
 * All access must be protected by g_data_mutex. Never read or write
 * any field without first acquiring the mutex via xSemaphoreTake().
 *
 * Sentinel values:
 *   - 0    = sensor still acquiring a lock (heart_rate, breathe_rate only)
 *   - 0xFF = field not applicable — no presence detected
 */
typedef struct
{
    /** @brief Presence detection result from the C1001.
     *         0 = no one detected, 1 = person present. */
    uint8_t presence;

    /** @brief Heart rate in BPM from the C1001 mmWave radar.
     *         0 = sensor still acquiring lock.
     *         0xFF = no presence detected. */
    uint8_t heart_rate;

    /** @brief Breathing rate in BPM from the C1001 mmWave radar.
     *         0 = sensor still acquiring lock.
     *         0xFF = no presence detected. */
    uint8_t breathe_rate;

    /** @brief Number of apnea events detected by the C1001.
     *         0xFF = no presence detected. */
    uint8_t apnea_events;

    /** @brief Sleep disturbance classification from the C1001.
     *         0 = sleep duration less than 4 hours.
     *         1 = sleep duration more than 12 hours.
     *         2 = abnormal absence detected.
     *         3 = no disturbance (normal).
     *         0xFF = no presence detected. */
    uint8_t sleep_disturbance;

    /** @brief True when heart_rate and breathe_rate contain valid locked readings.
     *
     *  Set by c1001_task when presence == 1 and both heart_rate and breathe_rate
     *  are non-zero and not 0xFF. The payload task skips sending until this is true
     *  to avoid transmitting invalid vitals during the sensor acquisition period. */
    bool vitals_locked;

    /** @brief Room temperature in degrees Celsius from the DS18B20 sensor.
     *
     *  Updated approximately every 1 second by ds18b20_task.
     *  Readings outside -10.0 to 85.0°C are discarded as invalid. */
    float temperature_c;

    /** @brief Human-readable temperature zone string from classify_temp_zone().
     *
     *  One of: "Too Cold", "Cool", "Ideal", "Warm", "Too Hot".
     *  Written via snprintf() — always null-terminated within 16 bytes. */
    char temp_zone[16];

} SleepData_t;

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Global shared sensor data instance.
 *
 * Defined and zero-initialised in main.c. Shared across all tasks.
 * Must only be accessed while holding g_data_mutex.
 */
extern SleepData_t g_sleep_data;

/**
 * @brief Mutex protecting g_sleep_data against concurrent task access.
 *
 * Created in app_main() before any tasks are started. All three tasks
 * must call xSemaphoreTake() before accessing g_sleep_data and
 * xSemaphoreGive() immediately after to release it.
 */
extern SemaphoreHandle_t g_data_mutex;

// ─────────────────────────────────────────────────────────────────────────────
// Helper
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Classify a temperature reading into a sleep zone string.
 *
 * Maps a temperature in Celsius to one of five zone labels based on
 * recommended sleep environment temperature ranges:
 *
 * | Range            | Zone     |
 * |------------------|----------|
 * | Below 16.0°C     | Too Cold |
 * | 16.0 – 17.9°C    | Cool     |
 * | 18.0 – 21.9°C    | Ideal    |
 * | 22.0 – 25.9°C    | Warm     |
 * | 26.0°C and above | Too Hot  |
 *
 * Returns a pointer to a string literal — must not be freed or modified.
 * Safe to copy into SleepData_t.temp_zone via snprintf().
 *
 * Defined in main.c.
 *
 * @param temp_c  Temperature in degrees Celsius.
 *
 * @return Pointer to a null-terminated zone string literal.
 */
const char *classify_temp_zone(float temp_c);

#endif // SLEEP_DATA_H