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

// --- BLE device name ---
#define BLE_DEVICE_NAME     "SleepMonitor"

// --- Public API ---

/** @brief Initialize the NimBLE stack, register GATT service,
 *         and start advertising.
 *  @return 0 on success, non-zero on failure */
int  BLE_init(void);

/** @brief Check if a client is currently connected AND has subscribed
 *         to notifications.
 *  @return true if ready to send, false otherwise */
bool BLE_is_connected(void);

/** @brief Send a JSON string to the connected client.
 *         Splits the string into MTU-sized chunks and notifies each one.
 *         Sends a 0x04 (EOT) byte after the last chunk so the client
 *         can reliably detect end-of-message regardless of payload length.
 *  @param json     Null-terminated JSON string to send
 *  @param json_len Length of the JSON string in bytes
 *  @return 0 on success, non-zero if not connected/subscribed or send failed */
int  BLE_send_payload(const char *json, size_t json_len);

#endif // BLE_SERVER_H