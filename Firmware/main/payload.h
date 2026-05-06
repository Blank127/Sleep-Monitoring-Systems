/**
 * @file payload.h
 * @brief JSON packet builders for the Sleep Monitor BLE payloads.
 *
 * Provides functions to serialise system state into the JSON packet formats
 * expected by the C# BLE server (PacketParser.cs).
 *
 * Two packet types are defined by the protocol:
 *
 *   session_start — sent once on boot to signal a new session:
 * @code
 *   {"type":"session_start"}
 * @endcode
 *
 *   reading — sent every 10s with averaged sensor data:
 * @code
 *   {"type":"reading","heart_rate":72,"breathe_rate":16,
 *    "temperature_c":21.50,"temp_zone":"Ideal",
 *    "apnea_events":0,"sleep_disturbance":3}
 * @endcode
 *
 * Both functions follow the snprintf() convention — they write into a
 * caller-provided buffer and return the number of bytes written, or -1
 * if the buffer was too small. JSON_BUF_SIZE (defined in sleep_data.h)
 * is large enough for the longest possible reading packet.
 */

#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stddef.h>
#include "sleep_data.h"

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Build a session_start JSON packet into the provided buffer.
 *
 * Serialises a fixed packet that signals the start of a new BLE session
 * to the C# server. The server uses this to create a new Session record
 * in the database before any reading packets arrive.
 *
 * @param buf      Destination buffer for the null-terminated JSON string.
 * @param buf_len  Size of @p buf in bytes. Must be at least 24 bytes.
 *
 * @return Number of bytes written (excluding null terminator) on success.
 * @return -1 if @p buf was too small to hold the complete packet.
 */
int build_session_start_packet(char *buf, size_t buf_len);

/**
 * @brief Build a reading JSON packet from a SleepData_t snapshot.
 *
 * Serialises all sensor fields from @p data into a JSON reading packet.
 * The caller is responsible for ensuring @p data contains valid averaged
 * readings — this function performs no range or null checking on the data.
 *
 * @param buf      Destination buffer for the null-terminated JSON string.
 * @param buf_len  Size of @p buf in bytes. JSON_BUF_SIZE (256) is sufficient
 *                 for the longest possible reading packet.
 * @param data     Pointer to the SleepData_t snapshot to serialise.
 *                 Must not be NULL.
 *
 * @return Number of bytes written (excluding null terminator) on success.
 * @return -1 if @p buf was too small to hold the complete packet.
 */
int build_reading_packet(char *buf, size_t buf_len, const SleepData_t *data);

#endif // PAYLOAD_H