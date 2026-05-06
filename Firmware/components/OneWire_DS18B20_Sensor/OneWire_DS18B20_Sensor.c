/**
 * @file OneWire_DS18B20_Sensor.c
 * @brief ESP-IDF driver implementation for the DS18B20 temperature sensor.
 *
 * Implements the 1-Wire protocol in software by bit-banging a single GPIO pin.
 * All timing is derived from the DS18B20 datasheet and handled via
 * esp_rom_delay_us() for microsecond-level precision without FreeRTOS overhead.
 *
 * The protocol follows the standard 1-Wire transaction sequence:
 *   1. Reset pulse — master holds bus low for 480µs
 *   2. Presence pulse — sensor pulls low for 60–240µs to confirm presence
 *   3. ROM command — SKIP ROM (0xCC) to address the single sensor on the bus
 *   4. Function command — CONVERT T (0x44) or READ SCRATCHPAD (0xBE)
 *   5. Data exchange — read or write bytes LSB first
 */

#include "OneWire_DS18B20_Sensor.h"
#include "esp_rom_sys.h"

// ─────────────────────────────────────────────────────────────────────────────
// DS18B20 ROM commands
// ─────────────────────────────────────────────────────────────────────────────

/** @def DS18B20_CMD_SKIP_ROM
 *  @brief Skip the 64-bit ROM addressing step.
 *
 *  Valid only when a single sensor is on the bus. Allows the master to
 *  issue function commands without first reading and matching the ROM code.
 */
#define DS18B20_CMD_SKIP_ROM        0xCC

/** @def DS18B20_CMD_CONVERT_T
 *  @brief Trigger a temperature conversion.
 *
 *  Commands the sensor to begin measuring. The result is stored in the
 *  scratchpad after up to 750ms (12-bit resolution).
 */
#define DS18B20_CMD_CONVERT_T       0x44

/** @def DS18B20_CMD_READ_SCRATCHPAD
 *  @brief Read the 9-byte scratchpad memory.
 *
 *  Bytes 0 and 1 contain the temperature LSB and MSB respectively.
 *  The remaining 7 bytes contain alarm thresholds, configuration,
 *  and a CRC — not read by this driver.
 */
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE

// ─────────────────────────────────────────────────────────────────────────────
// 1-Wire timing constants (microseconds)
// All values are derived from the DS18B20 datasheet timing requirements.
// ─────────────────────────────────────────────────────────────────────────────

/** @def DELAY_RESET_PULSE
 *  @brief Master holds bus low to issue a reset pulse (µs). */
#define DELAY_RESET_PULSE    480

/** @def DELAY_PRESENCE_WAIT
 *  @brief Master waits after releasing before sampling for presence pulse (µs). */
#define DELAY_PRESENCE_WAIT   70

/** @def DELAY_PRESENCE_READ
 *  @brief Remaining reset window after presence sample — must expire before
 *         the next transaction begins (µs). */
#define DELAY_PRESENCE_READ  410

/** @def DELAY_WRITE_1_LOW
 *  @brief Short low pulse to write a logic 1 bit (µs). */
#define DELAY_WRITE_1_LOW     10

/** @def DELAY_WRITE_1_HIGH
 *  @brief Recovery time after writing a logic 1 bit (µs). */
#define DELAY_WRITE_1_HIGH    55

/** @def DELAY_WRITE_0_LOW
 *  @brief Long low pulse to write a logic 0 bit (µs). */
#define DELAY_WRITE_0_LOW     65

/** @def DELAY_WRITE_0_HIGH
 *  @brief Recovery time after writing a logic 0 bit (µs). */
#define DELAY_WRITE_0_HIGH     5

/** @def DELAY_READ_LOW
 *  @brief Master pulls bus low to initiate a read slot (µs). */
#define DELAY_READ_LOW         5

/** @def DELAY_READ_SAMPLE
 *  @brief Time after releasing before sampling the line (µs).
 *
 *  The sensor must have driven the line to a valid level by this point.
 *  Sampling too early may read an invalid level during the bus transition. */
#define DELAY_READ_SAMPLE     10

/** @def DELAY_READ_HIGH
 *  @brief Recovery time to complete the read slot (µs). */
#define DELAY_READ_HIGH       55

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Delay for the specified number of microseconds.
 *
 * Wraps esp_rom_delay_us() which uses a hardware cycle counter for
 * microsecond precision without suspending the FreeRTOS scheduler.
 *
 * @param us  Number of microseconds to delay.
 */
static void delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

/**
 * @brief Pull the 1-Wire bus low.
 *
 * Switches ONE_WIRE_PIN to output mode and drives it to logic 0.
 * Must be followed by release_line() within the timing window
 * required by the current bit operation.
 */
static void drive_low(void)
{
    gpio_set_direction(ONE_WIRE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ONE_WIRE_PIN, 0);
}

/**
 * @brief Release the 1-Wire bus.
 *
 * Switches ONE_WIRE_PIN back to input mode. The external 4.7kΩ pull-up
 * resistor pulls the line back to 3.3V. The sensor can then drive the
 * line low to send a 0 bit or presence pulse.
 */
static void release_line(void)
{
    gpio_set_direction(ONE_WIRE_PIN, GPIO_MODE_INPUT);
}

/**
 * @brief Sample the current logic level on the 1-Wire bus.
 *
 * @return 1 if the bus is high (logic 1 or idle).
 * @return 0 if the bus is low (driven by master or sensor).
 */
static uint8_t read_line(void)
{
    return (uint8_t)gpio_get_level(ONE_WIRE_PIN);
}

/**
 * @brief Write a single bit to the 1-Wire bus.
 *
 * A logic 1 is written with a short low pulse (10µs) followed by a long
 * recovery period — the sensor samples during the low pulse and sees a 1.
 *
 * A logic 0 is written with a long low pulse (65µs) followed by a short
 * recovery — the sensor samples during the extended low and sees a 0.
 *
 * All timing values match the DS18B20 datasheet write slot requirements.
 *
 * @param bit  Bit value to write. Non-zero is treated as logic 1.
 */
static void DS18B20_WriteBit(uint8_t bit)
{
    if (bit)
    {
        drive_low();
        delay_us(DELAY_WRITE_1_LOW);
        release_line();
        delay_us(DELAY_WRITE_1_HIGH);
    }
    else
    {
        drive_low();
        delay_us(DELAY_WRITE_0_LOW);
        release_line();
        delay_us(DELAY_WRITE_0_HIGH);
    }
}

/**
 * @brief Read a single bit from the 1-Wire bus.
 *
 * Master initiates the read slot with a short low pulse (5µs), then releases.
 * The sensor drives the line low for a 0 or lets it float high for a 1.
 * Master samples the line 10µs after releasing, then waits 55µs to complete
 * the read slot before the next operation begins.
 *
 * @return 1 if the sensor drove the bus high (logic 1).
 * @return 0 if the sensor held the bus low (logic 0).
 */
static uint8_t DS18B20_ReadBit(void)
{
    drive_low();
    delay_us(DELAY_READ_LOW);
    release_line();
    delay_us(DELAY_READ_SAMPLE);
    uint8_t bit = read_line();
    delay_us(DELAY_READ_HIGH);  // Complete the read slot before the next operation
    return bit;
}

/**
 * @brief Write a full byte to the 1-Wire bus, LSB first.
 *
 * The 1-Wire protocol transmits bytes LSB first. Each bit is written
 * using DS18B20_WriteBit() with the appropriate timing.
 *
 * @param byte  Byte value to transmit.
 */
static void DS18B20_WriteByte(uint8_t byte)
{
    for (int i = 0; i < 8; i++)
    {
        DS18B20_WriteBit(byte & 0x01);  // Transmit LSB
        byte >>= 1;                      // Shift next bit into LSB position
    }
}

/**
 * @brief Read a full byte from the 1-Wire bus, LSB first.
 *
 * The 1-Wire protocol transmits bytes LSB first. Each bit is read using
 * DS18B20_ReadBit() and assembled into a byte from LSB to MSB.
 *
 * @return The byte received from the sensor.
 */
static uint8_t DS18B20_ReadByte(void)
{
    uint8_t data = 0;
    for (int i = 0; i < 8; i++)
    {
        data >>= 1;                     // Shift existing bits right to make room
        if (DS18B20_ReadBit())
        {
            data |= 0x80;               // Place new bit at MSB position
        }
    }
    return data;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Issue a 1-Wire reset pulse and check for a presence pulse.
 *
 * Pulls the bus low for 480µs, then releases and waits 70µs before sampling.
 * A connected DS18B20 will pull the bus low during this window to signal
 * presence. The function then waits the remaining 410µs to complete the
 * reset window before returning.
 *
 * @return true  if a presence pulse was detected.
 * @return false if the bus stayed high (sensor absent or wiring fault).
 */
bool DS18B20_Reset(void)
{
    drive_low();
    delay_us(DELAY_RESET_PULSE);
    release_line();
    delay_us(DELAY_PRESENCE_WAIT);
    uint8_t presence = !read_line();    // Low = sensor present (invert for true/false)
    delay_us(DELAY_PRESENCE_READ);      // Wait out the remainder of the reset window
    return presence;
}

/**
 * @brief Configure the 1-Wire GPIO pin and verify the sensor is present.
 *
 * Configures ONE_WIRE_PIN as an input with the internal pull-up enabled,
 * then issues a reset pulse via DS18B20_Reset() to confirm the sensor
 * is connected and responding.
 *
 * @note The internal pull-up (~45kΩ) is enabled as a backup only.
 *       The external 4.7kΩ resistor is still required for reliable
 *       1-Wire operation — without it the bus rise time is too slow.
 *
 * @return true  if the sensor responded with a presence pulse.
 * @return false if no presence pulse was detected.
 */
bool DS18B20_Init(void)
{
    gpio_config_t cfg =
    {
        .pin_bit_mask = (1ULL << ONE_WIRE_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     // Backup only — external 4.7kΩ required
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    return DS18B20_Reset();
}

/**
 * @brief Command the DS18B20 to begin a temperature conversion.
 *
 * Issues a reset pulse, then sends SKIP ROM (0xCC) followed by
 * CONVERT T (0x44). The sensor begins measuring immediately and stores
 * the result in its scratchpad after up to 750ms (12-bit resolution).
 *
 * @note The caller must wait at least 750ms after this function returns
 *       before calling DS18B20_ReadTemperature(). Calling earlier will
 *       return the result of the previous conversion.
 */
void DS18B20_StartConversion(void)
{
    DS18B20_Reset();
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);    // Address single sensor without ROM code
    DS18B20_WriteByte(DS18B20_CMD_CONVERT_T);   // Start temperature measurement
}

/**
 * @brief Read the most recent temperature conversion result.
 *
 * Issues a reset pulse, then sends SKIP ROM (0xCC) followed by
 * READ SCRATCHPAD (0xBE). Reads bytes 0 and 1 from the scratchpad
 * (temperature LSB and MSB), combines them into a signed 16-bit raw
 * value, and converts to degrees Celsius.
 *
 * The DS18B20 stores temperature as a fixed-point value with 4 fractional
 * bits — each LSB represents 0.0625°C (1/16°C). A raw value of 0x01F4
 * (500 decimal) represents 500 × 0.0625 = 31.25°C.
 *
 * @note Must be called at least 750ms after DS18B20_StartConversion().
 *
 * @return Temperature in degrees Celsius as a float (resolution 0.0625°C).
 */
float DS18B20_ReadTemperature(void)
{
    DS18B20_Reset();
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);        // Address single sensor
    DS18B20_WriteByte(DS18B20_CMD_READ_SCRATCHPAD); // Request scratchpad data

    uint8_t temp_lsb = DS18B20_ReadByte();          // Byte 0: temperature LSB
    uint8_t temp_msb = DS18B20_ReadByte();          // Byte 1: temperature MSB

    // Combine into a signed 16-bit fixed-point value.
    // Bits [3:0] are the fractional part, bits [10:4] are the integer part,
    // and bit [15] is the sign (two's complement for negative temperatures).
    int16_t temp_raw = (int16_t)((temp_msb << 8) | temp_lsb);

    // Each LSB = 0.0625°C (1/16°C)
    return (float)temp_raw * 0.0625f;
}