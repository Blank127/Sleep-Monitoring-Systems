#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "UART_C1001_Sensor.h"
#include "OneWire_DS18B20_Sensor.h"

static const char *TAG = "MAIN";

// --- Shared data struct ---
typedef struct 
{
    uint8_t  presence;
    uint8_t  heart_rate;       // 0xFF = no presence, 0 = acquiring
    uint8_t  breathe_rate;     // 0xFF = no presence, 0 = acquiring
    bool     vitals_locked;    // true = heart rate and breathe rate are valid
    float    temperature_c;
    char     temp_zone[16];
} SleepData_t;

// --- Globals ---
SleepData_t       g_sleep_data  = {0};
SemaphoreHandle_t g_data_mutex  = NULL;

// --- C1001 Task ---
void c1001_task(void *pvParameters)
{
    // Init sensor
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
            bool vitals_locked = (data.presence == 1) &&
                                 (data.heart_rate != 0) &&
                                 (data.heart_rate != 0xFF) &&
                                 (data.breathe_rate != 0) &&
                                 (data.breathe_rate != 0xFF);

            if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
            {
                g_sleep_data.presence      = data.presence;
                g_sleep_data.heart_rate    = data.heart_rate;
                g_sleep_data.breathe_rate  = data.breathe_rate;
                g_sleep_data.vitals_locked = vitals_locked;
                xSemaphoreGive(g_data_mutex);
            }
            else
            {
                ESP_LOGW(TAG, "C1001 task: could not take mutex");
            }

            // Log current state
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
                    ESP_LOGI(TAG, "Vitals      : %s", vitals_locked ? "Locked" : "Acquiring...");

                    if (vitals_locked)
                    {
                        ESP_LOGI(TAG, "Heart Rate  : %d BPM", data.heart_rate);
                        ESP_LOGI(TAG, "Breathe Rate: %d BPM", data.breathe_rate);
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
    // Create mutex before starting any tasks
    g_data_mutex = xSemaphoreCreateMutex();
    if (g_data_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    xTaskCreate(
        c1001_task,     // function
        "c1001_task",   // name for debugging
        4096,           // stack size in bytes
        NULL,           // parameters
        5,              // priority
        NULL            // handle, not needed
    );
}