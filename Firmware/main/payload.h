#ifndef PAYLOAD_H
#define PAYLOAD_H

#include <stddef.h>
#include "sleep_data.h"

/** @brief Build a session_start JSON packet into buf.
 *  @return Number of bytes written, -1 if buf was too small */
int build_session_start_packet(char *buf, size_t buf_len);

/** @brief Build a reading JSON packet into buf from a snapshot.
 *  @return Number of bytes written, -1 if buf was too small */
int build_reading_packet(char *buf, size_t buf_len,
                          const SleepData_t *data);

#endif // PAYLOAD_H