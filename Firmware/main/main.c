#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "UART_C1001_Sensor.h"
#include "OneWire_DS18B20_Sensor.h"

static const char *TAG = "MAIN";

void sensor_task(void *pvParameters)
{
    // --- Init C1001 ---
    esp_err_t ret = C1001_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "C1001 init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "C1001 init OK");

    // --- Init DS18B20 ---
    if (!DS18B20_Init())
    {
        ESP_LOGE(TAG, "DS18B20 not found — check wiring and pull-up resistor");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DS18B20 init OK");

    // Poll loop
    C1001_Sensor_Data_t data;
    while (true)
    {
        // --- Read C1001 ---
        memset(&data, 0, sizeof(data));
        ret = C1001_get_data(&data);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "C1001 read failed: %s", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "--- C1001 Data ---");

            switch (data.presence)
            {
                case 0:
                    ESP_LOGI(TAG, "Presence    : No one");
                    break;
                case 1:
                    ESP_LOGI(TAG, "Presence    : Someone present");
                    break;
                default:
                    ESP_LOGI(TAG, "Presence    : Read error");
                    break;
            }

            if (data.presence != 1)
            {
                ESP_LOGI(TAG, "Heart Rate  : N/A (no presence)");
                ESP_LOGI(TAG, "Breathe Rate: N/A (no presence)");
            }
            else
            {
                if (data.heart_rate == 0xFF || data.heart_rate == 0)
                    ESP_LOGI(TAG, "Heart Rate  : Acquiring...");
                else
                    ESP_LOGI(TAG, "Heart Rate  : %d BPM", data.heart_rate);

                if (data.breathe_rate == 0)
                    ESP_LOGI(TAG, "Breathe Rate: Acquiring...");
                else
                    ESP_LOGI(TAG, "Breathe Rate: %d BPM", data.breathe_rate);
            }
        }

        // --- Read DS18B20 ---
        // Trigger conversion, wait 750ms for 12-bit result, then read
        DS18B20_StartConversion();
        vTaskDelay(pdMS_TO_TICKS(750));
        float temp = DS18B20_ReadTemperature();

        ESP_LOGI(TAG, "--- DS18B20 Data ---");

        // Basic sanity check — DS18B20 valid range is -55°C to +125°C
        if (temp < -55.0f || temp > 125.0f)
        {
            ESP_LOGE(TAG, "Temperature : Out of range (%.2f C) — check sensor", temp);
        }
        else
        {
            ESP_LOGI(TAG, "Temperature : %.2f C", temp);
        }

        ESP_LOGI(TAG, "-------------------");

        // Total loop delay accounts for the 750ms conversion wait above
        // Adding 250ms here gives a ~1s cycle overall
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_main(void)
{
    xTaskCreate(
        sensor_task,    // function
        "sensor_task",  // name for debugging
        4096,           // stack size in bytes
        NULL,           // parameters
        5,              // priority
        NULL            // handle, not needed
    );
}