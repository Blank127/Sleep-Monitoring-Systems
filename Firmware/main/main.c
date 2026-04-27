#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "UART_C1001_Sensor.h"

static const char *TAG = "MAIN";

void sensor_task(void *pvParameters)
{
    // Init sensor
    esp_err_t ret = C1001_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Sensor init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Sensor init OK");

    // Poll loop
    C1001_Sensor_Data_t data;
    while (true)
    {
        memset(&data, 0, sizeof(data));

        ret = C1001_get_data(&data);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "--- Sensor Data ---");

            // Presence
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

            // Heart rate
            if (data.presence != 1)
            {
                ESP_LOGI(TAG, "Heart Rate  : N/A (no presence)");
            }
            else if (data.heart_rate == 0xFF || data.heart_rate == 0)
            {
                ESP_LOGI(TAG, "Heart Rate  : Acquiring...");
            }
            else
            {
                ESP_LOGI(TAG, "Heart Rate  : %d BPM", data.heart_rate);
            }

            // Breathing rate
            if (data.presence != 1)
            {
                ESP_LOGI(TAG, "Breathe Rate: N/A (no presence)");
            }
            else if (data.breathe_rate == 0)
            {
                ESP_LOGI(TAG, "Breathe Rate: Acquiring...");
            }
            else
            {
                ESP_LOGI(TAG, "Breathe Rate: %d BPM", data.breathe_rate);
            }

            ESP_LOGI(TAG, "-------------------");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}