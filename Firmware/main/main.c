#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sleep_data.h"
#include "tasks.h"
#include "BLE_Server.h"

static const char *TAG = "MAIN";

// --- Global definitions (declared extern in sleep_data.h) ---
SleepData_t       g_sleep_data = {0};
SemaphoreHandle_t g_data_mutex = NULL;

// --- classify_temp_zone definition ---
const char *classify_temp_zone(float temp_c)
{
    if (temp_c < 16.0f) return "Too Cold";
    if (temp_c < 18.0f) return "Cool";
    if (temp_c < 22.0f) return "Ideal";
    if (temp_c < 26.0f) return "Warm";
    return "Too Hot";
}

void app_main(void)
{
    g_data_mutex = xSemaphoreCreateMutex();
    if (g_data_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    if (BLE_init() != 0)
    {
        ESP_LOGE(TAG, "BLE init failed");
        return;
    }

    xTaskCreate(c1001_task,   "c1001_task",   4096, NULL, 5, NULL);
    xTaskCreate(ds18b20_task, "ds18b20_task", 2048, NULL, 4, NULL);
    xTaskCreate(payload_task, "payload_task", 4096, NULL, 3, NULL);
}