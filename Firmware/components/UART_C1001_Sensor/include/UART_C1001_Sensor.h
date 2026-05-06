/**
 * @file UART_C1001_Sensor.h
 * @brief ESP-IDF driver for the C1001 mmWave human presence and vitals sensor.
 *
 * Communicates with the C1001 over UART using the sensor's binary packet protocol.
 * In sleep monitoring mode the sensor provides:
 *   - Human presence detection
 *   - Heart rate (BPM)
 *   - Breathing rate (BPM)
 *   - Apnea event count
 *   - Sleep disturbance classification
 *
 * Typical usage:
 *   1. Call C1001_init() once at startup from a dedicated FreeRTOS task —
 *      it blocks for approximately 30 seconds during sensor boot and configuration
 *   2. Call C1001_get_data() in a loop to poll the latest sensor readings
 *
 * @note Heart rate and breathing rate return 0 while the sensor is acquiring
 *       a lock. This is normal for the first 30–60 seconds after presence
 *       is detected. Check that both values are non-zero and non-0xFF before
 *       treating them as valid readings.
 */

#ifndef UART_C1001_SENSOR_H
#define UART_C1001_SENSOR_H

#include <stdint.h>
#include "driver/uart.h"
#include "driver/gpio.h"

// ─────────────────────────────────────────────────────────────────────────────
// UART configuration
// ─────────────────────────────────────────────────────────────────────────────

/** @def SENSOR_UART_PORT
 *  @brief UART peripheral used to communicate with the C1001.
 *
 *  UART2 is used to avoid conflicts with the default UART0 (console)
 *  and UART1 (typically reserved for flash).
 */
#define SENSOR_UART_PORT    UART_NUM_2

/** @def SENSOR_TX_PIN
 *  @brief GPIO pin connected to the C1001 RX line (ESP32 transmits). */
#define SENSOR_TX_PIN       GPIO_NUM_4

/** @def SENSOR_RX_PIN
 *  @brief GPIO pin connected to the C1001 TX line (ESP32 receives). */
#define SENSOR_RX_PIN       GPIO_NUM_5

/** @def SENSOR_BAUD_RATE
 *  @brief UART baud rate required by the C1001 binary protocol. */
#define SENSOR_BAUD_RATE    115200

/** @def SENSOR_RX_BUF_SIZE
 *  @brief UART receive buffer size in bytes.
 *
 *  Must be large enough to hold at least one full response packet.
 *  The longest C1001 response is well under 32 bytes, so 1024 bytes
 *  provides comfortable headroom against burst traffic.
 */
#define SENSOR_RX_BUF_SIZE  1024

/** @def SENSOR_TX_BUF_SIZE
 *  @brief UART transmit buffer size in bytes. */
#define SENSOR_TX_BUF_SIZE  1024

// ─────────────────────────────────────────────────────────────────────────────
// Sensor data structure
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Sensor readings populated by C1001_get_data().
 *
 * Sentinel values are used to distinguish between different error conditions:
 *   - 0    = sensor is still acquiring a lock (heart_rate, breathe_rate only)
 *   - 0xFF = field is not applicable (no presence) or a read error occurred
 *
 * Always check @c presence == 1 before using any vitals fields.
 */
typedef struct
{
    /** @brief Presence detection result.
     *         0 = no one detected, 1 = person present. */
    uint8_t presence;

    /** @brief Heart rate in BPM.
     *         0 = sensor still acquiring lock.
     *         0xFF = no presence or not applicable. */
    uint8_t heart_rate;

    /** @brief Breathing rate in BPM.
     *         0 = sensor still acquiring lock.
     *         0xFF = no presence or not applicable. */
    uint8_t breathe_rate;

    /** @brief Number of apnea events detected during the current session.
     *         0xFF = read error or no presence. */
    uint8_t apnea_events;

    /** @brief Sleep disturbance classification code.
     *         0 = sleep duration less than 4 hours.
     *         1 = sleep duration more than 12 hours.
     *         2 = abnormal absence detected.
     *         3 = no disturbance (normal).
     *         0xFF = read error or no presence. */
    uint8_t sleep_disturbance;

} C1001_Sensor_Data_t;

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Initialise UART and configure the C1001 sensor for sleep monitoring mode.
 *
 * Performs the full sensor initialisation sequence:
 *   1. Configures UART2 with the required baud rate and pin assignments
 *   2. Waits ~10s for the sensor to complete its boot sequence
 *   3. Pings the sensor to confirm UART communication is working
 *   4. Reads the current work mode and switches to sleep mode (0x02) if needed
 *   5. Resets the sensor to apply the new configuration
 *   6. Waits ~10s for the sensor to reinitialise after reset
 *
 * @note This function blocks for approximately 30 seconds on the first call.
 *       It must be called from a dedicated FreeRTOS task, not from app_main()
 *       directly, to avoid starving other tasks during the boot delay.
 *
 * @return ESP_OK             on success.
 * @return ESP_ERR_TIMEOUT    if the sensor does not respond to the initial ping.
 */
esp_err_t C1001_init(void);

/**
 * @brief Read the latest presence and vitals data from the C1001 sensor.
 *
 * Queries the sensor over UART in the following order:
 *   1. Presence — if no one is detected, all vitals are set to 0xFF and
 *      the function returns ESP_OK without querying further
 *   2. Heart rate
 *   3. Breathing rate
 *   4. Apnea events
 *   5. Sleep disturbance classification
 *
 * Each query uses the binary packet protocol with checksum validation
 * and automatic retransmission if no valid response arrives within 1 second.
 *
 * @note Heart rate and breathing rate will be 0 while the sensor is acquiring
 *       a lock. Check @c vitals_locked in the calling task before using them.
 *
 * @param out  Pointer to a C1001_Sensor_Data_t struct to populate.
 *
 * @return ESP_OK               on success.
 * @return ESP_ERR_INVALID_ARG  if @p out is NULL.
 * @return ESP_ERR_TIMEOUT      if any individual sensor query times out.
 */
esp_err_t C1001_get_data(C1001_Sensor_Data_t *out);

#endif // UART_C1001_SENSOR_H