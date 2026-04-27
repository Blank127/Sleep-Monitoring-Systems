#include "payload.h"
#include <stdio.h>

int build_session_start_packet(char *buf, size_t buf_len)
{
    int written = snprintf(buf, buf_len,
        "{\"type\":\"session_start\"}"
    );

    if (written < 0 || (size_t)written >= buf_len)
    {
        return -1;
    }

    return written;
}

int build_reading_packet(char *buf, size_t buf_len,
                          const SleepData_t *data)
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

    if (written < 0 || (size_t)written >= buf_len)
    {
        return -1;
    }

    return written;
}