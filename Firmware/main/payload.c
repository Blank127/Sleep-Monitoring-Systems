/**
 * @file payload.c
 * @brief JSON packet builder for the Sleep Monitor BLE payloads.
 *
 * Provides two functions that serialise system state into the JSON packet
 * formats expected by the C# BLE server (PacketParser.cs):
 *
 *   - build_session_start_packet() — sent once on boot to signal a new session
 *   - build_reading_packet()       — sent every 10s with averaged sensor data
 *
 * Both functions write into a caller-provided buffer and return the number
 * of bytes written, following the same convention as snprintf(). The output
 * is always null-terminated if the buffer is large enough.
 *
 * Expected JSON formats:
 * @code
 *   {"type":"session_start"}
 *
 *   {"type":"reading","heart_rate":72,"breathe_rate":16,
 *    "temperature_c":21.50,"temp_zone":"Ideal",
 *    "apnea_events":0,"sleep_disturbance":3}
 * @endcode
 */

#include "payload.h"
#include <stdio.h>

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Build a session_start JSON packet into the provided buffer.
 *
 * Serialises a fixed packet that the C# server uses to detect the start
 * of a new BLE session and create a new Session record in the database.
 *
 * Output format:
 * @code
 *   {"type":"session_start"}
 * @endcode
 *
 * @param buf      Destination buffer for the null-terminated JSON string.
 * @param buf_len  Size of @p buf in bytes. Must be at least 24 bytes to
 *                 hold the full packet and null terminator.
 *
 * @return Number of bytes written (excluding null terminator) on success.
 * @return -1 if @p buf was too small to hold the complete packet.
 */
int build_session_start_packet(char *buf, size_t buf_len)
{
    int written = snprintf(buf, buf_len, "{\"type\":\"session_start\"}");

    // snprintf returns a negative value on encoding error, or a value >= buf_len
    // if the output was truncated. Either case means the packet is unusable.
    if (written < 0 || (size_t)written >= buf_len)
    {
        return -1;
    }

    return written;
}

/**
 * @brief Build a reading JSON packet from a SleepData_t snapshot.
 *
 * Serialises all sensor fields from @p data into a JSON string.
 * The caller is responsible for ensuring @p data contains valid,
 * averaged readings — this function performs no range checking.
 *
 * Output format:
 * @code
 *   {"type":"reading","heart_rate":72,"breathe_rate":16,
 *    "temperature_c":21.50,"temp_zone":"Ideal",
 *    "apnea_events":0,"sleep_disturbance":3}
 * @endcode
 *
 * Field mapping from SleepData_t:
 *   - heart_rate        → averaged over the last 10s window by payload_task
 *   - breathe_rate      → averaged over the last 10s window by payload_task
 *   - temperature_c     → most recent DS18B20 reading, formatted to 2 decimal places
 *   - temp_zone         → string from classify_temp_zone() e.g. "Ideal", "Too Hot"
 *   - apnea_events      → raw count from the C1001 sleep composite query
 *   - sleep_disturbance → classification code (0–3) from the C1001
 *
 * @param buf      Destination buffer for the null-terminated JSON string.
 * @param buf_len  Size of @p buf in bytes. JSON_BUF_SIZE (256) is sufficient
 *                 for the longest possible packet.
 * @param data     Pointer to the SleepData_t snapshot to serialise.
 *                 Must not be NULL — the caller should validate before calling.
 *
 * @return Number of bytes written (excluding null terminator) on success.
 * @return -1 if @p buf was too small to hold the complete packet.
 */
int build_reading_packet(char *buf, size_t buf_len, const SleepData_t *data)
{
    int written = snprintf(buf, buf_len,
        "{"
            "\"type\":\"reading\","
            "\"heart_rate\":%d,"
            "\"breathe_rate\":%d,"
            "\"temperature_c\":%.2f,"
            "\"temp_zone\":\"%s\","
            "\"apnea_events\":%d,"
            "\"sleep_disturbance\":%d"
        "}",
        data->heart_rate,
        data->breathe_rate,
        data->temperature_c,
        data->temp_zone,
        data->apnea_events,
        data->sleep_disturbance
    );

    // snprintf returns a negative value on encoding error, or a value >= buf_len
    // if the output was truncated. A truncated JSON packet is invalid — discard it.
    if (written < 0 || (size_t)written >= buf_len)
    {
        return -1;
    }

    return written;
}

/**
 * @brief Build a session_end JSON packet into the provided buffer.
 *
 * @param buf      Destination buffer for the null-terminated JSON string.
 * @param buf_len  Size of @p buf in bytes.
 *
 * @return Number of bytes written on success, -1 if buf was too small.
 */
int build_session_end_packet(char *buf, size_t buf_len)
{
    int written = snprintf(buf, buf_len, "{\"type\":\"session_end\"}");
    if (written < 0 || (size_t)written >= buf_len)
    {
        return -1;
    }
    return written;
}