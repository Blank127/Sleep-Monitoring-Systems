#ifndef TASKS_H
#define TASKS_H

/** @brief FreeRTOS task — reads C1001 sensor and updates g_sleep_data */
void c1001_task(void *pvParameters);

/** @brief FreeRTOS task — reads DS18B20 sensor and updates g_sleep_data */
void ds18b20_task(void *pvParameters);

/** @brief FreeRTOS task — builds JSON payload and sends over BLE */
void payload_task(void *pvParameters);

#endif // TASKS_H