/**
 * @file tasks.c
 * @brief FreeRTOS task implementations for the Sleep Monitor firmware.
 *
 * Contains the three tasks that run concurrently for the lifetime of the
 * application:
 *
 *   - payload_task  — fires every 10s, averages accumulated C1001 samples,
 *                     and transmits a JSON reading over BLE
 *   - ds18b20_task  — triggers a DS18B20 conversion every ~1s and writes
 *                     the result into g_sleep_data
 *   - c1001_task    — polls the mmWave radar every ~1s, writes presence and
 *                     vitals into g_sleep_data, and accumulates HR/BR samples
 *                     for the payload task to average
 *
 * All three tasks share g_sleep_data protected by g_data_mutex. The mutex
 * is held for the minimum time necessary — shared data is read into a local
 * snapshot under the mutex, then the mutex is released before any further
 * processing.
 *
 * Data flow:
 * @code
 *   c1001_task   ──┐
 *                  ├──► g_sleep_data (mutex protected) ──► payload_task ──► BLE
 *   ds18b20_task ──┘
 * @endcode
 */

#include "tasks.h"
#include "sleep_data.h"
#include "payload.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "UART_C1001_Sensor.h"
#include "OneWire_DS18B20_Sensor.h"
#include "BLE_Server.h"

static const char *TAG = "MAIN";

// ─────────────────────────────────────────────────────────────────────────────
// Averaging accumulators
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Running sum of heart rate samples accumulated since the last payload send.
 *
 * Incremented by c1001_task on every valid locked reading.
 * Divided by sample_count and reset to 0 by payload_task every 10 seconds.
 * Must only be accessed while holding g_data_mutex.
 */
static int32_t hr_sum = 0;

/**
 * @brief Running sum of breathing rate samples accumulated since the last payload send.
 *
 * Incremented by c1001_task on every valid locked reading.
 * Divided by sample_count and reset to 0 by payload_task every 10 seconds.
 * Must only be accessed while holding g_data_mutex.
 */
static int32_t br_sum = 0;

/**
 * @brief Number of valid locked C1001 samples accumulated since the last payload send.
 *
 * The C1001 task samples every ~1s so roughly 6–7 samples are accumulated
 * per 10s payload window. Only incremented when vitals_locked is true —
 * acquiring samples (heart_rate == 0) are excluded to keep the average clean.
 * Reset to 0 by payload_task after each averaging step.
 * Must only be accessed while holding g_data_mutex.
 */
static int32_t sample_count = 0;

/// @brief Tracks whether presence was detected in the previous payload cycle.
///        Used to detect transitions between present and absent.
static bool was_present = false;

/// @brief Counts consecutive payload cycles with no presence.
///        Session end is only sent after NO_PRESENCE_THRESHOLD cycles.
static uint8_t no_presence_count = 0;

/// @brief Number of consecutive no-presence cycles before session_end is sent.
///        At 10s per cycle, 3 cycles = 30 seconds.
#define NO_PRESENCE_THRESHOLD 3

// ─────────────────────────────────────────────────────────────────────────────
// Payload task
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief FreeRTOS task — assembles averaged sensor readings into JSON and sends over BLE.
 *
 * On startup, immediately sends a session_start packet so the C# server can
 * open a new session record before any readings arrive.
 *
 * Main loop (fires every 10 seconds):
 *   1. Acquires g_data_mutex
 *   2. Averages accumulated HR/BR samples from c1001_task into g_sleep_data
 *   3. Resets the averaging accumulators for the next window
 *   4. Takes a local snapshot of g_sleep_data and releases the mutex
 *   5. Checks presence — if absent, increments no_presence_count
 *   6. If no_presence_count reaches NO_PRESENCE_THRESHOLD (30s), sends session_end
 *   7. If presence just returned, sends session_start to open a new session
 *   8. Skips the cycle if vitals not yet locked
 *   9. Builds a JSON reading packet from the snapshot
 *   10. Sends the packet over BLE if a client is connected
 *
 * The snapshot pattern (copy under mutex, process outside) keeps the mutex
 * held for the minimum possible time.
 *
 * @param pvParameters  Unused FreeRTOS task parameter.
 */
void payload_task(void *pvParameters)
{
    (void)pvParameters;
    char json_buf[JSON_BUF_SIZE];

    // Send session_start immediately on boot so the server knows a new
    // session has begun before any reading packets arrive
    int len = build_session_start_packet(json_buf, sizeof(json_buf));
    if (len > 0)
    {
        ESP_LOGI(TAG, "SESSION START: %s", json_buf);
        if (BLE_is_connected())
        {
            BLE_send_payload(json_buf, len);
        }
    }

    while (true)
    {
        // Wait 10 seconds before each reading cycle
        vTaskDelay(pdMS_TO_TICKS(10000));

        SleepData_t snapshot;
        if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            // If valid samples were collected during the 10s window,
            // average them and write back into g_sleep_data before snapshotting.
            if (sample_count > 0)
            {
                g_sleep_data.heart_rate   = (uint8_t)(hr_sum / sample_count);
                g_sleep_data.breathe_rate = (uint8_t)(br_sum / sample_count);
                ESP_LOGI(TAG, "Averaged over %d samples — HR: %d BPM, BR: %d BPM", sample_count, g_sleep_data.heart_rate, g_sleep_data.breathe_rate);
            }

            // Reset accumulators so the next 10s window starts fresh
            hr_sum       = 0;
            br_sum       = 0;
            sample_count = 0;

            // Copy shared data into a local snapshot, then release the mutex
            snapshot = g_sleep_data;
            xSemaphoreGive(g_data_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "Payload task: could not take mutex");
            continue;
        }

        // Determine if presence is fully confirmed with locked vitals
        bool currently_present = (snapshot.presence == 1) && snapshot.vitals_locked;

        // ── Presence lost ─────────────────────────────────────────────────────
        if (!currently_present)
        {
            no_presence_count++;

            ESP_LOGI(TAG, "Payload: no presence (%d/%d cycles)", no_presence_count, NO_PRESENCE_THRESHOLD);

            // Only send session_end after NO_PRESENCE_THRESHOLD consecutive
            // cycles with no presence — avoids closing the session on a brief
            // detection gap (e.g. person shifts position)
            if (no_presence_count >= NO_PRESENCE_THRESHOLD && was_present)
            {
                len = build_session_end_packet(json_buf, sizeof(json_buf));
                if (len > 0)
                {
                    ESP_LOGI(TAG, "SESSION END: %s", json_buf);
                    if (BLE_is_connected())
                    {
                        BLE_send_payload(json_buf, len);
                    }
                }

                was_present       = false;
                no_presence_count = 0;
            }

            continue;
        }

        // ── Presence confirmed ────────────────────────────────────────────────
        // Reset the no-presence counter since someone is here
        no_presence_count = 0;

        // Person just arrived or returned — send session_start
        if (!was_present)
        {
            len = build_session_start_packet(json_buf, sizeof(json_buf));
            if (len > 0)
            {
                ESP_LOGI(TAG, "SESSION START: %s", json_buf);
                if (BLE_is_connected())
                {
                    BLE_send_payload(json_buf, len);
                }
            }
            was_present = true;
        }

        // ── Skip if vitals not yet locked ─────────────────────────────────────
        if (!snapshot.vitals_locked)
        {
            ESP_LOGI(TAG, "Payload: vitals acquiring, skipping");
            continue;
        }

        // ── Build and send reading packet ─────────────────────────────────────
        len = build_reading_packet(json_buf, sizeof(json_buf), &snapshot);
        if (len > 0)
        {
            ESP_LOGI(TAG, "PAYLOAD: %s", json_buf);
            if (BLE_is_connected())
            {
                BLE_send_payload(json_buf, len);
            }
            else
            {
                ESP_LOGI(TAG, "No BLE client connected, payload not sent");
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DS18B20 task
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief FreeRTOS task — reads room temperature from the DS18B20 sensor.
 *
 * Initialises the sensor on startup. If initialisation fails the task
 * deletes itself — there is no recovery path without the sensor.
 *
 * Main loop (~1s cycle):
 *   1. Triggers a temperature conversion (DS18B20_StartConversion)
 *   2. Waits 800ms for the 12-bit conversion to complete (spec: 750ms)
 *   3. Reads the result and discards readings outside -10 to 85°C
 *   4. Classifies the temperature into a zone string
 *   5. Writes temperature_c and temp_zone into g_sleep_data under the mutex
 *
 * @param pvParameters  Unused FreeRTOS task parameter.
 */
void ds18b20_task(void *pvParameters)
{
    (void)pvParameters;

    if (!DS18B20_Init())
    {
        ESP_LOGE(TAG, "DS18B20 init failed: sensor not detected");
        vTaskDelete(NULL);  // No recovery without the sensor — kill this task
        return;
    }

    ESP_LOGI(TAG, "DS18B20 init OK");

    while (true)
    {
        // Trigger a conversion and wait for it to complete.
        // The DS18B20 needs up to 750ms for 12-bit resolution — wait 800ms
        // to provide a safe margin above the datasheet maximum.
        DS18B20_StartConversion();
        vTaskDelay(pdMS_TO_TICKS(800));

        float temp_c = DS18B20_ReadTemperature();

        // The DS18B20 operating range is -55 to +125°C but valid room
        // temperatures are much narrower. Discard anything outside
        // -10 to 85°C as a sensor glitch or wiring fault.
        if (temp_c < -10.0f || temp_c > 85.0f)
        {
            ESP_LOGW(TAG, "DS18B20: suspicious reading %.2f C, discarding", temp_c);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        const char *zone = classify_temp_zone(temp_c);

        // Write temperature and zone into shared data under the mutex.
        // snprintf is used for temp_zone to guarantee null-termination
        // within the 16-byte field.
        if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            g_sleep_data.temperature_c = temp_c;
            snprintf(g_sleep_data.temp_zone, sizeof(g_sleep_data.temp_zone),
                     "%s", zone);
            xSemaphoreGive(g_data_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "DS18B20 task: could not take mutex");
        }

        ESP_LOGI(TAG, "--- DS18B20 ---");
        ESP_LOGI(TAG, "Temperature : %.2f C", temp_c);
        ESP_LOGI(TAG, "Zone        : %s", zone);

        // Short delay before starting the next conversion cycle
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// C1001 task
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief FreeRTOS task — polls the C1001 mmWave radar for presence and vitals.
 *
 * Initialises the sensor on startup (~30s blocking). If initialisation fails
 * the task deletes itself — there is no recovery path without the sensor.
 *
 * Main loop (~1s cycle):
 *   1. Calls C1001_get_data() to query presence, HR, BR, apnea, disturbance
 *   2. Determines vitals_locked — true only when presence == 1 and both
 *      heart_rate and breathe_rate are non-zero and non-0xFF
 *   3. Writes all fields into g_sleep_data under the mutex
 *   4. If vitals are locked, accumulates HR and BR into the averaging sums
 *      (hr_sum, br_sum, sample_count) for payload_task to consume
 *
 * Only locked samples are accumulated — samples where heart_rate == 0
 * (sensor still acquiring) are excluded to keep the average meaningful.
 *
 * @param pvParameters  Unused FreeRTOS task parameter.
 */
void c1001_task(void *pvParameters)
{
    (void)pvParameters;

    esp_err_t ret = C1001_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "C1001 init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);  // No recovery without the sensor — kill this task
        return;
    }

    ESP_LOGI(TAG, "C1001 init OK");

    C1001_Sensor_Data_t data;

    while (true)
    {
        memset(&data, 0, sizeof(data));
        ret = C1001_get_data(&data);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "C1001 read failed: %s", esp_err_to_name(ret));
        }
        else
        {
            // Vitals are locked only when all three conditions are met:
            //   1. Someone is present
            //   2. Heart rate is non-zero (not still acquiring) and not 0xFF
            //   3. Breathe rate is non-zero (not still acquiring) and not 0xFF
            // During the first ~30–60s after detection both rates are 0
            // while the sensor acquires a stable reading.
            bool vitals_locked = (data.presence    == 1)    &&
                                 (data.heart_rate   != 0)    &&
                                 (data.heart_rate   != 0xFF) &&
                                 (data.breathe_rate != 0)    &&
                                 (data.breathe_rate != 0xFF);

            if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                // Always update presence, vitals_locked, apnea, and disturbance
                // regardless of whether vitals are locked
                g_sleep_data.presence          = data.presence;
                g_sleep_data.vitals_locked     = vitals_locked;
                g_sleep_data.apnea_events      = data.apnea_events;
                g_sleep_data.sleep_disturbance = data.sleep_disturbance;

                // Only update HR/BR and accumulate when vitals are locked.
                // Excludes acquiring samples (heart_rate == 0) from the average
                // so the payload task always works with meaningful values.
                if (vitals_locked)
                {
                    g_sleep_data.heart_rate   = data.heart_rate;
                    g_sleep_data.breathe_rate = data.breathe_rate;

                    hr_sum += data.heart_rate;
                    br_sum += data.breathe_rate;
                    sample_count++;
                }

                xSemaphoreGive(g_data_mutex);
            }
            else
            {
                ESP_LOGW(TAG, "C1001 task: could not take mutex");
            }

            ESP_LOGI(TAG, "--- C1001 ---");

            switch (data.presence)
            {
                case 0:
                    ESP_LOGI(TAG, "Presence    : No one");
                    ESP_LOGI(TAG, "Heart Rate  : N/A");
                    ESP_LOGI(TAG, "Breathe Rate: N/A");
                    break;

                case 1:
                    ESP_LOGI(TAG, "Presence    : Someone present");
                    ESP_LOGI(TAG, "Vitals      : %s",
                             vitals_locked ? "Locked" : "Acquiring...");

                    if (vitals_locked)
                    {
                        ESP_LOGI(TAG, "Heart Rate  : %d BPM", data.heart_rate);
                        ESP_LOGI(TAG, "Breathe Rate: %d BPM", data.breathe_rate);
                        ESP_LOGI(TAG, "Apnea Events: %d",     data.apnea_events);

                        switch (data.sleep_disturbance)
                        {
                            case 0:  ESP_LOGI(TAG, "Disturbance : Sleep < 4hrs");     break;
                            case 1:  ESP_LOGI(TAG, "Disturbance : Sleep > 12hrs");    break;
                            case 2:  ESP_LOGI(TAG, "Disturbance : Abnormal absence"); break;
                            case 3:  ESP_LOGI(TAG, "Disturbance : None");             break;
                            default: ESP_LOGI(TAG, "Disturbance : N/A");              break;
                        }
                    }
                    break;

                default:
                    ESP_LOGI(TAG, "Presence    : Read error");
                    break;
            }
        }

        // Poll every ~1s — gives roughly 6–7 samples per 10s payload window
        // for a meaningful average without overloading the UART bus
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}