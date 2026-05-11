/**
 * @file UART_C1001_Sensor.c
 * @brief ESP-IDF driver implementation for the C1001 mmWave human presence
 *        and vitals sensor.
 *
 * Communicates with the C1001 over UART using its binary packet protocol.
 * All commands follow the same packet structure:
 *
 *   [0x53][0x59][CON][CMD][LEN_H][LEN_L][DATA...][CHECKSUM][0x54][0x43]
 *
 * Where:
 *   - CON      identifies the sensor subsystem being queried
 *   - CMD      identifies the specific query within that subsystem
 *   - LEN_H/L  is the 16-bit big-endian payload length
 *   - CHECKSUM is the sum of all preceding bytes masked to 8 bits
 *
 * send_cmd_recv() handles packet construction, transmission, response parsing,
 * checksum validation, and automatic retransmission — all other functions
 * in this file build on top of it.
 */

#include "UART_C1001_Sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "C1001";

// ─────────────────────────────────────────────────────────────────────────────
// Timing constants
// ─────────────────────────────────────────────────────────────────────────────

/** @def TIME_OUT_MS
 *  @brief Maximum time to wait for a valid response before giving up (ms).
 *
 *  If no valid response arrives within this window, send_cmd_recv() returns
 *  ESP_ERR_TIMEOUT. Without this limit the loop would block forever on a
 *  dead or disconnected sensor.
 */
#define TIME_OUT_MS     5000

/** @def RETRY_MS
 *  @brief Interval between retransmissions if no valid response arrives (ms).
 *
 *  If RETRY_MS elapses without receiving a valid response, the RX buffer
 *  is flushed and the command packet is retransmitted. This handles cases
 *  where the sensor missed the first transmission or returned a corrupt packet.
 */
#define RETRY_MS        1000

/** @def POLL_DELAY_MS
 *  @brief Maximum time to block waiting for a single byte from UART (ms).
 *
 *  Passed to uart_read_bytes() on each iteration of the receive loop.
 *  Short enough to check the timeout and retry conditions frequently,
 *  but long enough to avoid spinning the CPU unnecessarily.
 */
#define POLL_DELAY_MS   50

// ─────────────────────────────────────────────────────────────────────────────
// Packet framing constants
// ─────────────────────────────────────────────────────────────────────────────

/** @def PKT_HEAD0
 *  @brief First byte of every C1001 packet header (0x53 = 'S'). */
#define PKT_HEAD0   0x53

/** @def PKT_HEAD1
 *  @brief Second byte of every C1001 packet header (0x59 = 'Y'). */
#define PKT_HEAD1   0x59

/** @def PKT_END0
 *  @brief First byte of every C1001 packet footer (0x54 = 'T'). */
#define PKT_END0    0x54

/** @def PKT_END1
 *  @brief Second byte of every C1001 packet footer (0x43 = 'C'). */
#define PKT_END1    0x43

// ─────────────────────────────────────────────────────────────────────────────
// Response parser state machine
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief States for the byte-by-byte response packet parser.
 *
 * The parser processes one byte per iteration of the receive loop.
 * Each state represents the expected position within the packet structure.
 * Any unexpected byte resets the parser to STATE_WAIT to resync on the
 * next valid header.
 *
 * Packet structure mapped to states:
 * @code
 *   [0x53]     → STATE_WAIT   validates PKT_HEAD0
 *   [0x59]     → STATE_HEAD   validates PKT_HEAD1
 *   [CON]      → STATE_CON    validates control byte matches sent command
 *   [CMD]      → STATE_CMD    validates command byte matches sent command
 *   [LEN_H]    → STATE_LEN_H  captures high byte of payload length
 *   [LEN_L]    → STATE_LEN_L  captures low byte, transitions to data collection
 *   [DATA...]  → STATE_DATA   collects payload bytes then validates checksum
 *   [0x54]     → STATE_END_H  validates first end byte
 *   [0x43]     → STATE_END_L  validates second end byte — packet complete
 * @endcode
 */
typedef enum
{
    STATE_WAIT,     /**< Waiting for first header byte (0x53) */
    STATE_HEAD,     /**< Got 0x53, waiting for second header byte (0x59) */
    STATE_CON,      /**< Got header, waiting for control byte */
    STATE_CMD,      /**< Got control byte, waiting for command byte */
    STATE_LEN_H,    /**< Got command, waiting for high byte of payload length */
    STATE_LEN_L,    /**< Got length high byte, waiting for low byte */
    STATE_DATA,     /**< Collecting payload bytes then validating checksum */
    STATE_CHKSUM,   /**< Reserved — checksum is handled inside STATE_DATA */
    STATE_END_H,    /**< Got valid checksum, waiting for end byte 0x54 */
    STATE_END_L,    /**< Got 0x54, waiting for final end byte 0x43 */
} parse_state_t;

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Compute the C1001 packet checksum over a buffer.
 *
 * Sums all bytes in the buffer and returns the lowest 8 bits of the result.
 * This matches the checksum algorithm documented in the C1001 protocol spec.
 *
 * Used both to compute the checksum for outgoing packets and to validate
 * the checksum byte in incoming response packets.
 *
 * @param len  Number of bytes to sum.
 * @param buf  Pointer to the buffer to checksum.
 *
 * @return 8-bit checksum value.
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
 * @brief Send a command packet to the C1001 and receive the validated response.
 *
 * Constructs a full binary packet from the provided control byte, command byte,
 * and payload, then transmits it over UART. A byte-by-byte state machine parser
 * validates the response header, control/command echo, payload length, checksum,
 * and footer before returning.
 *
 * If no valid response arrives within RETRY_MS, the RX buffer is flushed and
 * the packet is retransmitted. If no valid response arrives within TIME_OUT_MS
 * total, the function returns ESP_ERR_TIMEOUT.
 *
 * Packet format (both TX and RX):
 * @code
 *   [0x53][0x59][CON][CMD][LEN_H][LEN_L][DATA...][CHECKSUM][0x54][0x43]
 * @endcode
 *
 * @param con           Control byte identifying the sensor subsystem.
 * @param cmd           Command byte identifying the specific query.
 * @param send_data     Pointer to payload bytes to include in the packet.
 * @param send_len      Number of payload bytes.
 * @param ret_buf       Buffer to store the complete response packet.
 * @param ret_buf_size  Size of ret_buf in bytes.
 *
 * @return ESP_OK           if a complete, valid response was received.
 * @return ESP_ERR_TIMEOUT  if no valid response arrived within TIME_OUT_MS.
 */
static esp_err_t send_cmd_recv(uint8_t con, uint8_t cmd, uint8_t *send_data, uint16_t send_len, uint8_t *ret_buf, uint16_t ret_buf_size)
{
    // ── Build outgoing packet ─────────────────────────────────────────────────
    uint8_t pkt[20];
    pkt[0] = PKT_HEAD0;                              // Header byte 1
    pkt[1] = PKT_HEAD1;                              // Header byte 2
    pkt[2] = con;                                    // Control byte (subsystem selector)
    pkt[3] = cmd;                                    // Command byte (query selector)
    pkt[4] = (send_len >> 8) & 0xFF;                 // Payload length high byte
    pkt[5] = send_len & 0xFF;                        // Payload length low byte
    memcpy(&pkt[6], send_data, send_len);             // Payload data
    pkt[6 + send_len] = checksum(6 + send_len, pkt); // Checksum over header + payload
    pkt[7 + send_len] = PKT_END0;                    // End byte 1
    pkt[8 + send_len] = PKT_END1;                    // End byte 2
    uint16_t pkt_len  = 9 + send_len;                // Total packet length in bytes

    // ── Timing setup ──────────────────────────────────────────────────────────
    uint32_t t_start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    // Set t_last_tx before t_start by RETRY_MS so the first transmission
    // fires immediately on the first loop iteration
    uint32_t t_last_tx = t_start - RETRY_MS;

    // ── Parser state ──────────────────────────────────────────────────────────
    parse_state_t state = STATE_WAIT;
    uint16_t data_len   = 0;  // Payload length parsed from the response header
    uint16_t data_count = 0;  // Number of payload bytes collected so far

    while (true)
    {
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // ── Overall timeout ───────────────────────────────────────────────────
        // Give up if no valid response arrives within TIME_OUT_MS.
        // Without this guard the loop blocks indefinitely on a dead sensor.
        if ((now - t_start) > TIME_OUT_MS)
        {
            ESP_LOGE(TAG, "Timeout con=0x%02X cmd=0x%02X", con, cmd);
            return ESP_ERR_TIMEOUT;
        }

        // ── Retransmit logic ──────────────────────────────────────────────────
        // If RETRY_MS has elapsed since the last transmission, flush any stale
        // RX bytes and retransmit. Flushing prevents the parser from resuming
        // mid-stream on a corrupt or partial response from the previous attempt.
        if ((now - t_last_tx) >= RETRY_MS)
        {
            uint8_t flush_byte;
            while (uart_read_bytes(SENSOR_UART_PORT, &flush_byte, 1, 0) > 0)
            {
                // Intentionally empty — draining stale bytes from the RX buffer
            }

            uart_write_bytes(SENSOR_UART_PORT, pkt, pkt_len);
            t_last_tx  = now;
            state      = STATE_WAIT;  // Reset parser for the fresh response
            data_count = 0;
        }

        // ── Read one byte ─────────────────────────────────────────────────────
        // Blocks for up to POLL_DELAY_MS — short enough to check timeout and
        // retry conditions frequently without busy-spinning the CPU.
        uint8_t byte;
        int rx = uart_read_bytes(SENSOR_UART_PORT, &byte, 1, pdMS_TO_TICKS(POLL_DELAY_MS));
        if (rx <= 0) continue;  // No byte available — loop back and check timers

        // ── Response parser state machine ─────────────────────────────────────
        // Processes one byte per iteration. Validates each field against the
        // expected packet structure. Any mismatch resets to STATE_WAIT to
        // resync on the next valid header.
        switch (state)
        {
            case STATE_WAIT:
                // Look for the first header byte to begin packet synchronisation
                if (byte == PKT_HEAD0)
                {
                    ret_buf[0] = byte;
                    state = STATE_HEAD;
                }
                break;

            case STATE_HEAD:
                // Confirm the second header byte — both must match to proceed
                if (byte == PKT_HEAD1)
                {
                    ret_buf[1] = byte;
                    state = STATE_CON;
                }
                else
                {
                    state = STATE_WAIT;  // Not a valid header — resync
                }
                break;

            case STATE_CON:
                // Confirm the response echoes the control byte we sent.
                // A mismatch means this is a response to a different command —
                // discard it and wait for the correct one.
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
                // Confirm the response echoes the command byte we sent
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
                // Capture the high byte of the response payload length
                ret_buf[4] = byte;
                data_len   = (uint16_t)byte << 8;
                state      = STATE_LEN_L;
                break;

            case STATE_LEN_L:
                // Capture the low byte — now we know exactly how many
                // data bytes to collect before expecting the checksum
                ret_buf[5] = byte;
                data_len  |= byte;
                data_count = 0;
                state      = STATE_DATA;
                break;

            case STATE_DATA:
                if (data_count < data_len)
                {
                    // Still collecting payload bytes — store if buffer allows
                    if ((6 + data_count) < ret_buf_size)
                    {
                        ret_buf[6 + data_count] = byte;
                    }
                    data_count++;
                }
                else
                {
                    // All payload bytes collected — this byte is the checksum.
                    // Validate it against the sum of all preceding packet bytes.
                    uint8_t expected = checksum(6 + data_len, ret_buf);
                    if (byte == expected)
                    {
                        ret_buf[6 + data_len] = byte;
                        state = STATE_END_H;  // Checksum valid — wait for footer
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Checksum mismatch — discarding packet");
                        state = STATE_WAIT;   // Corrupt packet — resync
                    }
                }
                break;

            case STATE_END_H:
                // Capture first footer byte (0x54)
                ret_buf[7 + data_len] = byte;
                state = STATE_END_L;
                break;

            case STATE_END_L:
                // Capture second footer byte (0x43) — packet is complete and valid
                ret_buf[8 + data_len] = byte;
                return ESP_OK;

            default:
                state = STATE_WAIT;
                break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Initialise UART and configure the C1001 sensor for sleep monitoring mode.
 *
 * Performs the full sensor initialisation sequence:
 *   1. Configures UART2 with the required baud rate and pin assignments
 *   2. Waits ~10s for the sensor to complete its boot sequence
 *   3. Pings the sensor (HP LED query) to confirm UART communication is working
 *   4. Reads the current work mode — switches to sleep mode (0x02) if not set
 *   5. Resets the sensor to commit the new configuration
 *   6. Waits ~10s for the sensor to reinitialise after reset
 *
 * @note This function blocks for approximately 30 seconds on the first call.
 *       Call it from a dedicated FreeRTOS task, not directly from app_main(),
 *       to avoid starving other tasks during the boot delays.
 *
 * @return ESP_OK           on success.
 * @return ESP_ERR_TIMEOUT  if the sensor does not respond to the initial ping.
 */
esp_err_t C1001_init(void)
{
    // Configure UART2 for 115200 8N1 with no hardware flow control
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

    // The C1001 requires ~10s after power-on before it accepts commands.
    // Sending commands before this window expires results in no response.
    vTaskDelay(pdMS_TO_TICKS(10000));

    // Ping the sensor by querying the HP LED state (CON=0x01, CMD=0x83).
    // This is the same approach used by the official Arduino library begin().
    // If this times out the sensor is not responding on UART.
    uint8_t query = 0x0F;
    uint8_t buf[16] = {0};
    esp_err_t ret = send_cmd_recv(0x01, 0x83, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Sensor ping failed");
        return ret;
    }

    // Query the current work mode (CON=0x02, CMD=0xA8).
    // buf[6] == 0x02 means sleep mode is already active — no change needed.
    uint8_t mode_buf[16] = {0};
    esp_err_t mode_ret = send_cmd_recv(0x02, 0xA8, &query, 1, mode_buf, sizeof(mode_buf));
    if (mode_ret == ESP_OK && mode_buf[6] != 0x02)
    {
        // Mode switch uses a fixed packet format — the standard send_cmd_recv()
        // query structure is not used here because the mode byte must be
        // included in the payload with a specific packet layout.
        uint8_t set_mode[10] = {0x53, 0x59, 0x02, 0x08, 0x00, 0x01, 0x02, 0x00, 0x54, 0x43};
        set_mode[7] = checksum(7, set_mode);  // Recalculate checksum including mode byte
        uart_write_bytes(SENSOR_UART_PORT, set_mode, 10);

        // Sensor needs ~10s to reinitialise after a mode switch
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    // Reset the sensor to commit all configuration changes.
    // Without this reset, settings such as work mode may not persist.
    uint8_t ret_buf[16] = {0};
    send_cmd_recv(0x01, 0x02, &query, 1, ret_buf, sizeof(ret_buf));

    // Wait for the sensor to come back up after the reset
    vTaskDelay(pdMS_TO_TICKS(10000));

    // Set unattended time to 10 seconds.
    // This controls how long the sensor holds the presence flag after
    // the last detection before reporting "no one present".
    // Default is several minutes — reducing it speeds up session end detection.
    uint8_t unattended_time = 10;
    uint8_t unattended_buf[16] = {0};
    send_cmd_recv(0x84, 0x15, &unattended_time, 1, unattended_buf, sizeof(unattended_buf));
    ESP_LOGI(TAG, "Unattended time set to %d seconds", unattended_time);

    ESP_LOGI(TAG, "C1001 init complete");
    return ESP_OK;
}

/**
 * @brief Read the latest presence and vitals data from the C1001 sensor.
 *
 * Queries the sensor subsystems in order. If no presence is detected,
 * all vitals fields are set to 0xFF and the function returns immediately
 * without issuing further queries.
 *
 * Query sequence when presence is detected:
 *   - Presence       CON=0x80, CMD=0x81 → out->presence      = buf[6]
 *   - Heart rate     CON=0x85, CMD=0x82 → out->heart_rate    = buf[6]
 *   - Breathe rate   CON=0x81, CMD=0x82 → out->breathe_rate  = buf[6]
 *   - Apnea events   CON=0x84, CMD=0x8D → out->apnea_events  = buf[13]
 *   - Disturbance    CON=0x84, CMD=0x8E → out->sleep_disturbance = buf[6]
 *
 * @note Heart rate and breathing rate return 0 while the sensor is still
 *       acquiring a lock. This is normal for the first 30–60 seconds after
 *       presence is detected. The calling task should check vitals_locked
 *       before using these values.
 *
 * @param out  Pointer to a C1001_Sensor_Data_t struct to populate.
 *
 * @return ESP_OK               on success.
 * @return ESP_ERR_INVALID_ARG  if @p out is NULL.
 * @return ESP_ERR_TIMEOUT      if any individual sensor query times out.
 */
esp_err_t C1001_get_data(C1001_Sensor_Data_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t query = 0x0F;
    uint8_t buf[22] = {0};
    esp_err_t ret;

    // ── Presence ──────────────────────────────────────────────────────────────
    ret = send_cmd_recv(0x80, 0x81, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->presence = buf[6];

    // If no one is present, vitals are not applicable — set sentinel values
    // and return without issuing any further queries
    if (out->presence != 1)
    {
        out->heart_rate        = 0xFF;
        out->breathe_rate      = 0xFF;
        out->apnea_events      = 0xFF;
        out->sleep_disturbance = 0xFF;
        return ESP_OK;
    }

    // ── Heart rate ────────────────────────────────────────────────────────────
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x85, 0x82, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->heart_rate = buf[6];

    // ── Breathing rate ────────────────────────────────────────────────────────
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x81, 0x82, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->breathe_rate = buf[6];

    // ── Apnea events ──────────────────────────────────────────────────────────
    // Apnea count is at buf[13] in the sleep composite response, not buf[6]
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x84, 0x8D, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->apnea_events = buf[13];

    // ── Sleep disturbance ─────────────────────────────────────────────────────
    memset(buf, 0, sizeof(buf));
    ret = send_cmd_recv(0x84, 0x8E, &query, 1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;
    out->sleep_disturbance = buf[6];

    return ESP_OK;
}