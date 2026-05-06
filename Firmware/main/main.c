/**
 * @file main.c
 * @brief Application entry point for the Sleep Monitor firmware.
 *
 * Initialises all shared resources and launches the three FreeRTOS tasks
 * that make up the system:
 *
 *   - c1001_task   — polls the mmWave radar for presence and vitals
 *   - ds18b20_task — reads room temperature from the DS18B20 sensor
 *   - payload_task — assembles averaged readings into JSON and sends over BLE
 *
 * All three tasks share a single SleepData_t struct (g_sleep_data) protected
 * by a FreeRTOS mutex (g_data_mutex). No task reads or writes g_sleep_data
 * without first acquiring this mutex.
 *
 * Task priority assignment:
 *   - c1001_task   priority 5 (highest) — sensor polling is time-sensitive
 *   - ds18b20_task priority 4           — temperature changes slowly, less urgent
 *   - payload_task priority 3 (lowest)  — only fires every 10s, not time-critical
 */

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sleep_data.h"
#include "tasks.h"
#include "BLE_Server.h"

static const char *TAG = "MAIN";

// ─────────────────────────────────────────────────────────────────────────────
// Global shared state
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Shared sensor data accessible by all three tasks.
 *
 * Written by c1001_task and ds18b20_task, read by payload_task.
 * Must only be accessed while holding g_data_mutex.
 * Zero-initialised at startup — all fields are overwritten before first use.
 */
SleepData_t g_sleep_data = {0};

/**
 * @brief Mutex protecting g_sleep_data against concurrent access.
 *
 * Created in app_main() before any tasks are started. Every task must
 * call xSemaphoreTake() before reading or writing g_sleep_data, and
 * xSemaphoreGive() immediately after to release it.
 */
SemaphoreHandle_t g_data_mutex = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// Temperature zone classification
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Classify a temperature reading into a human-readable sleep zone string.
 *
 * Thresholds are based on recommended sleep environment temperatures:
 *
 * | Range          | Zone       |
 * |----------------|------------|
 * | Below 16.0°C   | Too Cold   |
 * | 16.0 – 17.9°C  | Cool       |
 * | 18.0 – 21.9°C  | Ideal      |
 * | 22.0 – 25.9°C  | Warm       |
 * | 26.0°C and above | Too Hot  |
 *
 * The returned string is a string literal — it must not be freed or modified
 * by the caller. It is safe to copy into g_sleep_data.temp_zone via snprintf().
 *
 * @param temp_c  Temperature in degrees Celsius.
 *
 * @return Pointer to a null-terminated string literal describing the zone.
 */
const char *classify_temp_zone(float temp_c)
{
    if (temp_c < 16.0f) return "Too Cold";
    if (temp_c < 18.0f) return "Cool";
    if (temp_c < 22.0f) return "Ideal";
    if (temp_c < 26.0f) return "Warm";
    return "Too Hot";
}

// ─────────────────────────────────────────────────────────────────────────────
// Application entry point
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief ESP-IDF application entry point.
 *
 * Called by the ESP-IDF startup code after the system has booted.
 * Performs the following initialisation in order:
 *   1. Creates the shared data mutex — tasks must not start without it
 *   2. Initialises the NimBLE stack and starts BLE advertising
 *   3. Spawns the three sensor and payload FreeRTOS tasks
 *
 * If mutex creation or BLE initialisation fails, the function logs the
 * error and returns early. No tasks are created in this case.
 *
 * @note app_main() is called from a FreeRTOS task created by the ESP-IDF
 *       startup code. Returning from app_main() is safe — the scheduler
 *       continues running any tasks that were created before the return.
 */
void app_main(void)
{
    // Create the mutex before starting any tasks — all three tasks require
    // it to be valid before they attempt to access g_sleep_data
    g_data_mutex = xSemaphoreCreateMutex();
    if (g_data_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex — insufficient heap");
        return;
    }

    // Initialise the NimBLE stack and start BLE advertising.
    // Must complete before the payload task starts, as BLE_is_connected()
    // and BLE_send_payload() are called from payload_task.
    if (BLE_init() != 0)
    {
        ESP_LOGE(TAG, "BLE init failed");
        return;
    }

    // Spawn the three tasks. Each task runs indefinitely in its own loop.
    // Stack sizes are set conservatively above measured peak usage:
    //   c1001_task   4096 bytes — UART + protocol parser + FreeRTOS overhead
    //   ds18b20_task 2048 bytes — simple GPIO bit-bang, minimal stack needed
    //   payload_task 4096 bytes — JSON formatting + BLE send + snprintf
    xTaskCreate(c1001_task,   "c1001_task",   4096, NULL, 5, NULL);
    xTaskCreate(ds18b20_task, "ds18b20_task", 2048, NULL, 4, NULL);
    xTaskCreate(payload_task, "payload_task", 4096, NULL, 3, NULL);
}