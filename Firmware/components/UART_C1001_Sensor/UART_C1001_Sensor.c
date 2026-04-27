#include "UART_C1001_Sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "C1001";

// --- Timing constants ---
#define TIME_OUT_MS     5000    // Max time to wait for a valid response before giving up
#define RETRY_MS        1000    // Retransmit the command if no valid response within this window
#define POLL_DELAY_MS   50      // How long to block waiting for a single byte from UART

// --- Packet framing constants ---
// Every packet starts with 0x53 0x59 and ends with 0x54 0x43
#define PKT_HEAD0   0x53
#define PKT_HEAD1   0x59
#define PKT_END0    0x54
#define PKT_END1    0x43

// --- Response parser states ---
// The parser is a state machine that processes the response one byte at a time.
// Each state represents where we are in the expected packet structure.
typedef enum 
{
    STATE_WAIT,     // Waiting for first header byte (0x53)
    STATE_HEAD,     // Got 0x53, waiting for second header byte (0x59)
    STATE_CON,      // Got header, waiting for control byte to match what we sent
    STATE_CMD,      // Got control byte, waiting for command byte to match what we sent
    STATE_LEN_H,    // Got command, waiting for high byte of payload length
    STATE_LEN_L,    // Got length high byte, waiting for low byte
    STATE_DATA,     // Collecting payload bytes, then validating checksum
    STATE_CHKSUM,   // (Reserved - checksum handled inside STATE_DATA)
    STATE_END_H,    // Got valid checksum, waiting for end byte 0x54
    STATE_END_L,    // Got 0x54, waiting for final end byte 0x43
} parse_state_t;

/**
 * @brief Compute checksum over a buffer.
 *        Sum all bytes and return the lowest 8 bits.
 *        Matches the checksum algorithm used by the C1001 sensor.
 */
static uint8_t checksum(uint8_t len, uint8_t *buf) 
{
    uint16_t sum = 0;

    for (uint8_t i = 0; i < len; i++)
    {
        sum += buf[i];
    }

    return sum & 0xFF;
}

/**
 * @brief Send a command packet to the sensor and receive the response.
 *
 * Builds the full packet from the provided control byte (con), command byte (cmd),
 * and payload (send_data / send_len). Transmits it over UART, then runs a state
 * machine parser to validate and collect the response into ret_buf.
 *
 * Retransmits the command every RETRY_MS if no valid response arrives.
 * Returns ESP_ERR_TIMEOUT if no valid response is received within TIME_OUT_MS.
 *
 * Packet format (TX and RX):
 *   [0x53][0x59][CON][CMD][LEN_H][LEN_L][DATA...][CHECKSUM][0x54][0x43]
 *
 * @param con           Control byte - identifies the sensor subsystem
 * @param cmd           Command byte - identifies the specific query
 * @param send_data     Payload bytes to include in the packet
 * @param send_len      Number of payload bytes
 * @param ret_buf       Buffer to store the full response packet
 * @param ret_buf_size  Size of ret_buf in bytes
 * @return ESP_OK on success, ESP_ERR_TIMEOUT on failure
 */
static esp_err_t send_cmd_recv(uint8_t con, uint8_t cmd, uint8_t *send_data, uint16_t send_len, uint8_t *ret_buf, uint16_t ret_buf_size) 
{
    // Build the outgoing packet
    uint8_t pkt[20];
    pkt[0] = PKT_HEAD0;                         // Header byte 1
    pkt[1] = PKT_HEAD1;                         // Header byte 2
    pkt[2] = con;                               // Control byte (subsystem selector)
    pkt[3] = cmd;                               // Command byte (query selector)
    pkt[4] = (send_len >> 8) & 0xFF;            // Payload length high byte
    pkt[5] = send_len & 0xFF;                   // Payload length low byte
    memcpy(&pkt[6], send_data, send_len);        // Payload data
    pkt[6 + send_len] = checksum(6 + send_len, pkt); // Checksum over header + payload
    pkt[7 + send_len] = PKT_END0;               // End byte 1
    pkt[8 + send_len] = PKT_END1;               // End byte 2
    uint16_t pkt_len = 9 + send_len;            // Total packet length in bytes

    // Capture start time for overall timeout tracking
    uint32_t t_start   = xTaskGetTickCount() * portTICK_PERIOD_MS;
    
    // Set t_last_tx in the past so the first transmission happens immediately
    uint32_t t_last_tx = t_start - RETRY_MS;

    parse_state_t state = STATE_WAIT;
    uint16_t data_len   = 0;    // Payload length parsed from the response
    uint16_t data_count = 0;    // How many payload bytes collected so far

    while (true) 
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // --- Overall timeout check ---
        // If we've been waiting too long with no valid response, give up.
        // Without this the loop would block forever on a dead or disconnected sensor.
        if ((now - t_start) > TIME_OUT_MS) 
        {
            ESP_LOGE(TAG, "Timeout con=0x%02X cmd=0x%02X", con, cmd);
            return ESP_ERR_TIMEOUT;
        }

        // --- Retransmit logic ---
        // If RETRY_MS has passed since the last transmission, flush stale RX data
        // and retransmit the packet. Also resets the parser to a clean state.
        if ((now - t_last_tx) >= RETRY_MS) 
        {
            // Flush the RX buffer before retransmitting.
            // This discards any partial or corrupt response bytes that arrived
            // after the last failed attempt, preventing the parser from picking
            // up mid-stream data on the next pass.
            uint8_t flush_byte;
            while (uart_read_bytes(SENSOR_UART_PORT, &flush_byte, 1, 0) > 0)
            {
                // Intentionally empty — just draining the buffer
            }

            uart_write_bytes(SENSOR_UART_PORT, pkt, pkt_len);  // (Re)transmit command
            t_last_tx  = now;
            state      = STATE_WAIT;    // Reset parser for fresh response
            data_count = 0;
        }

        // --- Read one byte from UART ---
        // Blocks for up to POLL_DELAY_MS before returning 0 if no data arrives.
        uint8_t byte;
        int rx = uart_read_bytes(SENSOR_UART_PORT, &byte, 1, pdMS_TO_TICKS(POLL_DELAY_MS));
        if (rx <= 0) continue;  // No byte available yet, loop back

        // --- Response parser state machine ---
        // Processes one byte per iteration. Validates header, control byte,
        // command byte, payload length, data, checksum, and end bytes in order.
        // Any mismatch resets to STATE_WAIT to wait for the next valid header.
        switch (state) 
        {
            case STATE_WAIT:
                // Looking for the start of a valid packet
                if (byte == PKT_HEAD0) 
                {
                    ret_buf[0] = byte;
                    state = STATE_HEAD;
                }
                break;

            case STATE_HEAD:
                // Confirm the second header byte
                if (byte == PKT_HEAD1) 
                {
                    ret_buf[1] = byte;
                    state = STATE_CON;
                } 
                else 
                {
                    state = STATE_WAIT; // Not a valid header, start over
                }
                break;

            case STATE_CON:
                // Confirm the response is for the subsystem we queried
                if (byte == con) 
                {
                    ret_buf[2] = byte;
                    state = STATE_CMD;
                } 
                else 
                {
                    state = STATE_WAIT; // Response is for a different subsystem
                }
                break;

            case STATE_CMD:
                // Confirm the response is for the command we sent
                if (byte == cmd) 
                {
                    ret_buf[3] = byte;
                    state = STATE_LEN_H;
                } 
                else 
                {
                    state = STATE_WAIT; // Response is for a different command
                }
                break;

            case STATE_LEN_H:
                // High byte of the response payload length
                ret_buf[4] = byte;
                data_len   = (uint16_t)byte << 8;
                state      = STATE_LEN_L;
                break;

            case STATE_LEN_L:
                // Low byte of the response payload length — now we know how many data bytes to expect
                ret_buf[5] = byte;
                data_len  |= byte;
                data_count = 0;
                state      = STATE_DATA;
                break;

            case STATE_DATA:
                if (data_count < data_len) 
                {
                    // Still collecting payload bytes into ret_buf
                    if ((6 + data_count) < ret_buf_size) 
                    {
                        ret_buf[6 + data_count] = byte;
                    }
                    data_count++;
                } 
                else
                {
                    // All payload bytes collected — this byte should be the checksum
                    uint8_t expected = checksum(6 + data_len, ret_buf);
                    if (byte == expected) 
                    {
                        ret_buf[6 + data_len] = byte;
                        state = STATE_END_H;    // Checksum valid, wait for end bytes
                    } 
                    else 
                    {
                        ESP_LOGW(TAG, "Checksum mismatch");
                        state = STATE_WAIT;     // Corrupt packet, start over
                    }
                }
                break;

            case STATE_END_H:
                // First end byte (0x54)
                ret_buf[7 + data_len] = byte;
                state = STATE_END_L;
                break;

            case STATE_END_L:
                // Second end byte (0x43) — packet is complete and valid
                ret_buf[8 + data_len] = byte;
                return ESP_OK;

            default:
                state = STATE_WAIT;
                break;
        }
    }
}

static uint8_t C1001_get_apnea_events(void)
{
    uint8_t query = 0x0F;
    uint8_t buf[22] = {0};  // sleep composite response is longer than a single value

    // Query sleep composite (con=0x84, cmd=0x8D)
    // The composite struct is memcopied from buf[6] in the Arduino library:
    // [0] presence
    // [1] sleepState
    // [2] averageRespiration
    // [3] averageHeartbeat
    // [4] turnoverNumber
    // [5] largeBodyMove
    // [6] minorBodyMove
    // [7] apneaEvents      ← buf[6+7] = buf[13]
    esp_err_t ret = send_cmd_recv(0x84, 0x8D, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK)
    {
        ESP_LOGE("C1001", "get_apnea_events failed: %s", esp_err_to_name(ret));
        return 0xFF;
    }

    return buf[13];  // apneaEvents is the 8th field in the composite struct
}

static uint8_t C1001_get_sleep_disturbance(void)
{
    uint8_t query = 0x0F;
    uint8_t buf[16] = {0};

    // Query sleep disturbances (con=0x84, cmd=0x8E)
    // Response buf[6]: 0 = sleep < 4hrs, 1 = sleep > 12hrs,
    //                  2 = abnormal absence, 3 = none
    esp_err_t ret = send_cmd_recv(0x84, 0x8E, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK)
    {
        ESP_LOGE("C1001", "get_sleep_disturbance failed: %s", esp_err_to_name(ret));
        return 0xFF;
    }

    return buf[6];
}

// -------------------------------------------------------
// Public API
// -------------------------------------------------------

/**
 * @brief Initialize the C1001 sensor.
 *
 * Configures UART, waits for the sensor to boot, pings it to confirm
 * communication, switches it to sleep mode if not already set, then
 * resets it to commit the configuration.
 *
 * Blocks for approximately 30 seconds total across three vTaskDelay calls.
 * Call this from a dedicated task, not directly from app_main.
 *
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if the sensor does not respond
 */
esp_err_t C1001_init(void) 
{
    // Configure UART peripheral
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
    vTaskDelay(pdMS_TO_TICKS(10000)); // C1001 needs ~10s to boot before it accepts commands

    // Ping the sensor by querying the HP LED state (same approach as Arduino begin())
    // If this times out, the sensor is not responding
    uint8_t query = 0x0F;
    uint8_t buf[16] = {0};
    esp_err_t ret = send_cmd_recv(0x01, 0x83, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) 
    {
        ESP_LOGE(TAG, "Sensor ping failed");
        return ret;
    }

    // Query current work mode and switch to sleep mode (0x02) if not already set
    uint8_t mode_buf[16] = {0};
    esp_err_t mode_ret = send_cmd_recv(0x02, 0xA8, &query, 1, mode_buf, sizeof(mode_buf));
    if (mode_ret == ESP_OK && mode_buf[6] != 0x02) 
    {
        // Build and send the mode-switch command manually
        // (mode switch uses a fixed packet format, not the standard query structure)
        uint8_t set_mode[10] = {0x53, 0x59, 0x02, 0x08, 0x00, 0x01, 0x02, 0x00, 0x54, 0x43};
        set_mode[7] = checksum(7, set_mode);    // Recalculate checksum with mode byte included
        uart_write_bytes(SENSOR_UART_PORT, set_mode, 10);
        vTaskDelay(pdMS_TO_TICKS(10000));       // Sensor needs ~10s to reinitialize after mode switch
    }

    // Reset the sensor to commit all configuration changes
    // Without this reset, settings like work mode may not take effect
    uint8_t ret_buf[16] = {0};
    send_cmd_recv(0x01, 0x02, &query, 1, ret_buf, sizeof(ret_buf));
    vTaskDelay(pdMS_TO_TICKS(10000));           // Wait for sensor to come back up after reset

    ESP_LOGI(TAG, "C1001 init complete");
    return ESP_OK;
}

/**
 * @brief Read presence, heart rate, and breathing rate from the sensor.
 *
 * Queries presence first. If no one is detected, heart rate and breathing
 * rate are set to 0xFF (not applicable) without querying the sensor.
 * If presence is detected, both vitals are queried and returned.
 *
 * Values of 0 for heart rate or breathing rate indicate the sensor is still
 * acquiring a lock — this is normal for the first 30-60 seconds of detection.
 *
 * @param out  Pointer to struct to populate with sensor readings
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out is NULL,
 *         ESP_ERR_TIMEOUT if any query times out
 */
esp_err_t C1001_get_data(C1001_Sensor_Data_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t query = 0x0F;
    uint8_t buf[22] = {0};
    esp_err_t ret;

    // Presence
    ret = send_cmd_recv(0x80, 0x81, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->presence = buf[6];

    // Skip vitals if no one is present
    if (out->presence != 1)
    {
        out->heart_rate        = 0xFF;
        out->breathe_rate      = 0xFF;
        out->apnea_events      = 0xFF;
        out->sleep_disturbance = 0xFF;
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

    // Apnea events from sleep composite (buf[13])
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x84, 0x8D, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->apnea_events = buf[13];

    // Sleep disturbance flag
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x84, 0x8E, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->sleep_disturbance = buf[6];

    return ESP_OK;
}