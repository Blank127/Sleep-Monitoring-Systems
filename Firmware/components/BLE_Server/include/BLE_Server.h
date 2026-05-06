/**
 * @file BLE_Server.h
 * @brief NimBLE GATT server for the Sleep Monitor.
 *
 * Advertises as "SleepMonitor" and exposes one notify characteristic.
 * The payload task calls BLE_send_payload() to transmit JSON data
 * split into MTU-sized chunks.
 *
 * Typical usage:
 *   1. Call BLE_init() once at startup
 *   2. Call BLE_is_connected() before sending
 *   3. Call BLE_send_payload() with the JSON string — only sends if
 *      the client has subscribed to notifications
 */

#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** @defgroup BLE_Server BLE Server
 *  @brief NimBLE GATT server public interface.
 *  @{
 */

/** @def BLE_DEVICE_NAME
 *  @brief Advertised BLE device name.
 *
 *  Must match the name the C# client scans for in BleConstants.cs.
 */
#define BLE_DEVICE_NAME "SleepMonitor"

/**
 * @brief Initialize the NimBLE stack, register the GATT service,
 *        and start BLE advertising.
 *
 * Must be called once at startup before any other BLE function.
 * Internally initialises NVS, the NimBLE port, GAP and GATT services,
 * registers the custom 128-bit service and notify characteristic,
 * and starts the NimBLE host task.
 *
 * @return 0 on success, non-zero on failure.
 */
int BLE_init(void);

/**
 * @brief Check whether a client is connected and has subscribed to notifications.
 *
 * The payload task calls this before BLE_send_payload() to avoid
 * sending data to a client that hasn't enabled notifications yet.
 *
 * @return true  if a client is connected AND has subscribed to notifications.
 * @return false if no client is connected, or client has not subscribed.
 */
bool BLE_is_connected(void);

/**
 * @brief Send a JSON string to the connected BLE client.
 *
 * Splits the JSON string into MTU-sized chunks and sends each one as a
 * GATT notification. After the last chunk, sends a single 0x04 (EOT) byte
 * so the client can detect end-of-message regardless of payload length.
 *
 * Will fail silently if no client is connected or notifications are not
 * subscribed — check BLE_is_connected() first.
 *
 * @param json     Null-terminated JSON string to send.
 * @param json_len Length of the JSON string in bytes (excluding null terminator).
 *
 * @return 0 on success.
 * @return -1 if no client is connected or not subscribed.
 * @return Non-zero NimBLE error code if a notification send fails.
 */
int BLE_send_payload(const char *json, size_t json_len);

/** @} */ // end of BLE_Server group

#endif // BLE_SERVER_H