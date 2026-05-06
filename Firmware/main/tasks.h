/**
 * @file tasks.h
 * @brief FreeRTOS task declarations for the Sleep Monitor firmware.
 *
 * Declares the three tasks that run concurrently for the lifetime of the
 * application. All tasks are created in app_main() and run indefinitely.
 *
 * Task responsibilities and priorities:
 *
 * | Task          | Priority | Stack  | Responsibility                        |
 * |---------------|----------|--------|---------------------------------------|
 * | c1001_task    | 5        | 4096 B | Poll mmWave radar, accumulate HR/BR   |
 * | ds18b20_task  | 4        | 2048 B | Read room temperature every ~1s       |
 * | payload_task  | 3        | 4096 B | Average samples, build JSON, send BLE |
 *
 * All three tasks share g_sleep_data protected by g_data_mutex, both
 * declared in sleep_data.h.
 */

#ifndef TASKS_H
#define TASKS_H

// ─────────────────────────────────────────────────────────────────────────────
// Task declarations
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief FreeRTOS task — polls the C1001 mmWave radar for presence and vitals.
 *
 * Initialises the C1001 sensor on startup (~30s blocking). Polls every ~1s
 * and writes presence, heart rate, breathing rate, apnea events, and sleep
 * disturbance into g_sleep_data. Accumulates heart rate and breathing rate
 * samples into the averaging sums (hr_sum, br_sum) for payload_task to consume.
 *
 * Only accumulates samples when vitals_locked is true — excludes acquiring
 * samples (heart_rate == 0) from the average.
 *
 * Deletes itself if C1001_init() fails.
 *
 * @param pvParameters  Unused. Pass NULL when creating with xTaskCreate().
 */
void c1001_task(void *pvParameters);

/**
 * @brief FreeRTOS task — reads room temperature from the DS18B20 sensor.
 *
 * Initialises the DS18B20 sensor on startup. Triggers a temperature conversion
 * every ~1s, validates the result, classifies it into a zone string, and
 * writes temperature_c and temp_zone into g_sleep_data.
 *
 * Discards readings outside -10 to 85°C as invalid.
 * Deletes itself if DS18B20_Init() fails.
 *
 * @param pvParameters  Unused. Pass NULL when creating with xTaskCreate().
 */
void ds18b20_task(void *pvParameters);

/**
 * @brief FreeRTOS task — averages sensor samples and transmits JSON over BLE.
 *
 * Sends a session_start packet immediately on boot. Then fires every 10s:
 * averages accumulated HR/BR samples from c1001_task, snapshots g_sleep_data,
 * builds a JSON reading packet, and sends it over BLE if a client is connected
 * and presence is detected with locked vitals.
 *
 * @param pvParameters  Unused. Pass NULL when creating with xTaskCreate().
 */
void payload_task(void *pvParameters);

#endif // TASKS_H