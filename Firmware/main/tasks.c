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
        vTaskDelay(pdMS_TO_TICKS(800));

        float temp_c = DS18B20_ReadTemperature();

        // Discard obviously invalid readings
        if (temp_c < -10.0f || temp_c > 85.0f)
        {
            ESP_LOGW(TAG, "DS18B20: suspicious reading %.2f C, discarding",
                     temp_c);
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