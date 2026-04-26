#include "UART_C1001_Sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "C1001";

#define TIME_OUT_MS     5000
#define RETRY_MS        1000
#define POLL_DELAY_MS   50

// --- Packet constants ---
#define PKT_HEAD0   0x53
#define PKT_HEAD1   0x59
#define PKT_END0    0x54
#define PKT_END1    0x43

// --- Parser states ---
typedef enum {
    STATE_WAIT,
    STATE_HEAD,
    STATE_CON,
    STATE_CMD,
    STATE_LEN_H,
    STATE_LEN_L,
    STATE_DATA,
    STATE_CHKSUM,
    STATE_END_H,
    STATE_END_L,
} parse_state_t;

// --- Internal ---
static uint8_t checksum(uint8_t len, uint8_t *buf) 
{
    uint16_t sum = 0;

    for (uint8_t i = 0; i < len; i++)
    {
        sum += buf[i];
    }

    return sum & 0xFF;
}

static esp_err_t send_cmd_recv(uint8_t con, uint8_t cmd, uint8_t *send_data, uint16_t send_len, uint8_t *ret_buf, uint16_t ret_buf_size) 
{
    // Build packet
    uint8_t pkt[20];
    pkt[0] = PKT_HEAD0;
    pkt[1] = PKT_HEAD1;
    pkt[2] = con;
    pkt[3] = cmd;
    pkt[4] = (send_len >> 8) & 0xFF;
    pkt[5] = send_len & 0xFF;
    memcpy(&pkt[6], send_data, send_len);
    pkt[6 + send_len] = checksum(6 + send_len, pkt);
    pkt[7 + send_len] = PKT_END0;
    pkt[8 + send_len] = PKT_END1;
    uint16_t pkt_len = 9 + send_len;

    uint32_t t_start    = xTaskGetTickCount() * portTICK_PERIOD_MS;
    uint32_t t_last_tx  = t_start - RETRY_MS; // force immediate send

    parse_state_t state = STATE_WAIT;
    uint16_t data_len   = 0;
    uint16_t data_count = 0;

    while (true) 
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // Overall timeout
        if ((now - t_start) > TIME_OUT_MS) 
        {
            ESP_LOGE(TAG, "Timeout con=0x%02X cmd=0x%02X", con, cmd);
            return ESP_ERR_TIMEOUT;
        }

        // Retransmit every RETRY_MS
        if ((now - t_last_tx) >= RETRY_MS) 
        {
            // Flush RX before retransmit
            uint8_t flush_byte;
            while (uart_read_bytes(SENSOR_UART_PORT, &flush_byte, 1, 0) > 0)
            {

            }
            uart_write_bytes(SENSOR_UART_PORT, pkt, pkt_len);
            t_last_tx = now;
            state     = STATE_WAIT;
            data_count = 0;
        }

        // Read one byte
        uint8_t byte;
        int rx = uart_read_bytes(SENSOR_UART_PORT, &byte, 1, pdMS_TO_TICKS(POLL_DELAY_MS));
        if (rx <= 0) continue;

        // State machine
        switch (state) 
        {
            case STATE_WAIT:
                if (byte == PKT_HEAD0) 
                {
                    ret_buf[0] = byte;
                    state = STATE_HEAD;
                }
                break;

            case STATE_HEAD:
                if (byte == PKT_HEAD1) 
                {
                    ret_buf[1] = byte;
                    state = STATE_CON;
                } 
                else 
                {
                    state = STATE_WAIT;
                }
                break;

            case STATE_CON:
                if (byte == con) 
                {
                    ret_buf[2] = byte;
                    state = STATE_CMD;
                } 
                else 
                {
                    state = STATE_WAIT;
                }
                break;

            case STATE_CMD:
                if (byte == cmd) 
                {
                    ret_buf[3] = byte;
                    state = STATE_LEN_H;
                } 
                else 
                {
                    state = STATE_WAIT;
                }
                break;

            case STATE_LEN_H:
                ret_buf[4] = byte;
                data_len   = (uint16_t)byte << 8;
                state      = STATE_LEN_L;
                break;

            case STATE_LEN_L:
                ret_buf[5] = byte;
                data_len  |= byte;
                data_count = 0;
                state      = STATE_DATA;
                break;

            case STATE_DATA:
                if (data_count < data_len) 
                {
                    if ((6 + data_count) < ret_buf_size) 
                    {
                        ret_buf[6 + data_count] = byte;
                    }
                    data_count++;
                } 
                else // Stay in STATE_DATA until all bytes collected
                {
                    // This byte is the checksum
                    uint8_t expected = checksum(6 + data_len, ret_buf);
                    if (byte == expected) 
                    {
                        ret_buf[6 + data_len] = byte;
                        state = STATE_END_H;
                    } 
                    else 
                    {
                        ESP_LOGW(TAG, "Checksum mismatch");
                        state = STATE_WAIT;
                    }
                }
                break;

            case STATE_END_H:
                ret_buf[7 + data_len] = byte;
                state = STATE_END_L;
                break;

            case STATE_END_L:
                ret_buf[8 + data_len] = byte;
                return ESP_OK;

            default:
                state = STATE_WAIT;
                break;
        }
    }
}

// -------------------------------------------------------
// Public API
// -------------------------------------------------------

esp_err_t C1001_init(void) 
{
    uart_config_t cfg = 
    {
        .baud_rate  = SENSOR_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };

    ESP_ERROR_CHECK(uart_param_config(SENSOR_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(SENSOR_UART_PORT, SENSOR_TX_PIN, SENSOR_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SENSOR_UART_PORT, SENSOR_RX_BUF_SIZE, SENSOR_TX_BUF_SIZE, 0, NULL, 0));

    ESP_LOGI(TAG, "UART init done, waiting for sensor boot...");
    vTaskDelay(pdMS_TO_TICKS(10000)); // sensor boot time

    // Ping: query HP LED state
    uint8_t query = 0x0F;
    uint8_t buf[16] = {0};
    esp_err_t ret = send_cmd_recv(0x01, 0x83, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) 
    {
        ESP_LOGE(TAG, "Sensor ping failed");
        return ret;
    }

    // Set sleep mode
    uint8_t mode_buf[16] = {0};
    esp_err_t mode_ret = send_cmd_recv(0x02, 0xA8, &query, 1, mode_buf, sizeof(mode_buf));
    if (mode_ret == ESP_OK && mode_buf[6] != 0x02) 
    {
        uint8_t set_mode[10] = {0x53, 0x59, 0x02, 0x08, 0x00, 0x01, 0x02, 0x00, 0x54, 0x43};
        set_mode[7] = checksum(7, set_mode);
        uart_write_bytes(SENSOR_UART_PORT, set_mode, 10);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    // Reset to commit
    uint8_t ret_buf[16] = {0};
    send_cmd_recv(0x01, 0x02, &query, 1, ret_buf, sizeof(ret_buf));
    vTaskDelay(pdMS_TO_TICKS(10000));

    ESP_LOGI(TAG, "C1001 init complete");
    return ESP_OK;
}

esp_err_t C1001_get_data(C1001_Sensor_Data_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t query = 0x0F;
    uint8_t buf[16] = {0};
    esp_err_t ret;

    // Presence
    ret = send_cmd_recv(0x80, 0x81, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->presence = buf[6];

    // Only bother reading HR and BR if someone is present
    if (out->presence != 1)
    {
        out->heart_rate   = 0xFF;
        out->breathe_rate = 0xFF;
        return ESP_OK;
    }

    // Heart rate
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x85, 0x82, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->heart_rate = buf[6];

    // Breathing rate
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x81, 0x82, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->breathe_rate = buf[6];

    return ESP_OK;
}