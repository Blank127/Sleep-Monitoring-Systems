/**
 * @file BLE_Server.h
 * @brief NimBLE GATT server for the Sleep Monitor.
 *
 * Advertises as "SleepMonitor" and exposes one notify characteristic.
 * The payload task calls BLE_send_payload() to transmit JSON data
 * split into 20 byte chunks.
 *
 * Typical usage:
 *   1. Call BLE_init() once at startup
 *   2. Call BLE_is_connected() before sending
 *   3. Call BLE_send_payload() with the JSON string
 */
#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- BLE UUIDs ---
// 128-bit custom UUIDs for the Sleep Monitor service and characteristic
#define BLE_DEVICE_NAME     "SleepMonitor"

// --- Public API ---

/** @brief Initialize the NimBLE stack, register GATT service,
 *         and start advertising.
 *  @return 0 on success, non-zero on failure */
int  BLE_init(void);

/** @brief Check if a client is currently connected.
 *  @return true if connected, false otherwise */
bool BLE_is_connected(void);

/** @brief Send a JSON string to the connected client.
 *         Splits the string into 20 byte chunks and notifies each one.
 *         The client detects end of message when it receives a chunk
 *         smaller than 20 bytes.
 *  @param json     Null-terminated JSON string to send
 *  @param json_len Length of the JSON string in bytes
 *  @return 0 on success, non-zero if not connected or send failed */
int  BLE_send_payload(const char *json, int json_len);

#endif // BLE_SERVER_H