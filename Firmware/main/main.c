#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "UART_C1001_Sensor.h"
#include "OneWire_DS18B20_Sensor.h"
#include "BLE_Server.h"

static const char *TAG = "MAIN";

// --- JSON buffer size ---
// Worst case reading packet is ~150 bytes, 256 gives comfortable headroom
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

// --- Globals ---
SleepData_t       g_sleep_data = {0};
SemaphoreHandle_t g_data_mutex = NULL;

// --- Helpers ---
static const char *classify_temp_zone(float temp_c)
{
    if (temp_c < 16.0f) return "Too Cold";
    if (temp_c < 18.0f) return "Cool";
    if (temp_c < 22.0f) return "Ideal";
    if (temp_c < 26.0f) return "Warm";
    return "Too Hot";
}

/**
 * @brief Build a session_start JSON packet into buf.
 *        Called once at boot to tell the server a new session has begun.
 *
 * Output: {"type":"session_start"}
 *
 * @param buf     Buffer to write the JSON string into
 * @param buf_len Size of buf in bytes
 * @return Number of bytes written, -1 if buf was too small
 */
static int build_session_start_packet(char *buf, size_t buf_len)
{
    int written = snprintf(buf, buf_len,
        "{\"type\":\"session_start\"}"
    );

    if (written < 0 || (size_t)written >= buf_len)
    {
        return -1;
    }

    return written;
}

/**
 * @brief Build a reading JSON packet into buf from a snapshot of shared data.
 *        Only call this when vitals_locked is true.
 *
 * Output: {"type":"reading","heart_rate":62,"breathe_rate":15,
 *          "temperature_c":18.50,"temp_zone":"Ideal",
 *          "apnea_events":0,"sleep_disturbance":3}
 *
 * @param buf     Buffer to write the JSON string into
 * @param buf_len Size of buf in bytes
 * @param data    Snapshot of SleepData_t taken under mutex
 * @return Number of bytes written, -1 if buf was too small
 */
static int build_reading_packet(char *buf, size_t buf_len,
                                 const SleepData_t *data)
{
    int written = snprintf(buf, buf_len,
        "{"
            "\"type\":\"reading\","
            "\"heart_rate\":%d,"
            "\"breathe_rate\":%d,"
            "\"temperature_c\":%.2f,"
            "\"temp_zone\":\"%s\","
            "\"apnea_events\":%d,"
            "\"sleep_disturbance\":%d"
        "}",
        data->heart_rate,
        data->breathe_rate,
        data->temperature_c,
        data->temp_zone,
        data->apnea_events,
        data->sleep_disturbance
    );

    if (written < 0 || (size_t)written >= buf_len)
    {
        return -1;
    }

    return written;
}

// --- Payload Task ---
void payload_task(void *pvParameters)
{
    char json_buf[JSON_BUF_SIZE];

    // Send session_start on boot
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
        vTaskDelay(pdMS_TO_TICKS(10000));

        // Take a snapshot of shared data under mutex
        SleepData_t snapshot;
        if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            snapshot = g_sleep_data;
            xSemaphoreGive(g_data_mutex);
        }
        else
        {
            ESP_LOGW(TAG, "Payload task: could not take mutex");
            continue;
        }

        // Skip if no one present
        if (snapshot.presence != 1)
        {
            ESP_LOGI(TAG, "Payload: no presence, skipping");
            continue;
        }

        // Skip if vitals not yet locked
        if (!snapshot.vitals_locked)
        {
            ESP_LOGI(TAG, "Payload: vitals acquiring, skipping");
            continue;
        }

        // Build and send reading packet
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

// --- DS18B20 Task ---
void ds18b20_task(void *pvParameters)
{
    if (!DS18B20_Init())
    {
        ESP_LOGE(TAG, "DS18B20 init failed: sensor not detected");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DS18B20 init OK");

    while (true)
    {
        DS18B20_StartConversion();
        vTaskDelay(pdMS_TO_TICKS(800));  // 12-bit conversion needs ≥750ms

        float temp_c = DS18B20_ReadTemperature();
        // Discard obviously invalid readings
        // DS18B20 valid range is -55 to +125°C
        // A reading of 0 or below -10 during normal room use is corrupt
        if (temp_c < -10.0f || temp_c > 85.0f)
        {
            ESP_LOGW(TAG, "DS18B20: suspicious reading %.2f C, discarding", temp_c);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        
        const char *zone = classify_temp_zone(temp_c);

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

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// --- C1001 Task ---
void c1001_task(void *pvParameters)
{
    esp_err_t ret = C1001_init();

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "C1001 init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
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
            bool vitals_locked = (data.presence    == 1)    &&
                                 (data.heart_rate   != 0)    &&
                                 (data.heart_rate   != 0xFF) &&
                                 (data.breathe_rate != 0)    &&
                                 (data.breathe_rate != 0xFF);

            if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                g_sleep_data.presence          = data.presence;
                g_sleep_data.heart_rate        = data.heart_rate;
                g_sleep_data.breathe_rate      = data.breathe_rate;
                g_sleep_data.apnea_events      = data.apnea_events;
                g_sleep_data.sleep_disturbance = data.sleep_disturbance;
                g_sleep_data.vitals_locked     = vitals_locked;
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
                            case 0:
                                ESP_LOGI(TAG, "Disturbance : Sleep < 4hrs");
                                break;
                            case 1:
                                ESP_LOGI(TAG, "Disturbance : Sleep > 12hrs");
                                break;
                            case 2:
                                ESP_LOGI(TAG, "Disturbance : Abnormal absence");
                                break;
                            case 3:
                                ESP_LOGI(TAG, "Disturbance : None");
                                break;
                            default:
                                ESP_LOGI(TAG, "Disturbance : N/A");
                                break;
                        }
                    }
                    break;

                default:
                    ESP_LOGI(TAG, "Presence    : Read error");
                    break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    g_data_mutex = xSemaphoreCreateMutex();
    if (g_data_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    // Init BLE before starting tasks
    if (BLE_init() != 0)
    {
        ESP_LOGE(TAG, "BLE init failed");
        return;
    }

    xTaskCreate(c1001_task,   "c1001_task",   4096, NULL, 5, NULL);
    xTaskCreate(ds18b20_task, "ds18b20_task", 2048, NULL, 4, NULL);
    xTaskCreate(payload_task, "payload_task", 4096, NULL, 3, NULL);
}