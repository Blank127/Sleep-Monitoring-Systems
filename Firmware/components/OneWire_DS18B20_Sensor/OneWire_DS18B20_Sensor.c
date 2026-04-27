/**
 * @file OneWire_DS18B20_Sensor.c
 * @brief ESP-IDF driver implementation for the DS18B20 temperature sensor.
 *
 * Implements the 1-Wire protocol in software by bit-banging a single GPIO pin.
 * All timing is handled via esp_rom_delay_us() for microsecond precision.
 */

#include "OneWire_DS18B20_Sensor.h"
#include "esp_rom_sys.h"

// ── DS18B20 ROM commands ──────────────────────────────────────────────────────
#define DS18B20_CMD_SKIP_ROM        0xCC    // Address all sensors on the bus
#define DS18B20_CMD_CONVERT_T       0x44    // Start temperature conversion
#define DS18B20_CMD_READ_SCRATCHPAD 0xBE    // Read the 9-byte scratchpad memory

// ── 1-Wire timing constants (microseconds) ────────────────────────────────────
// All values are derived from the DS18B20 datasheet timing requirements.
#define DELAY_RESET_PULSE    480    // Master holds line low to reset the bus
#define DELAY_PRESENCE_WAIT   70    // Master releases then waits before sampling
#define DELAY_PRESENCE_READ  410    // Remaining reset window after presence sample

#define DELAY_WRITE_1_LOW     10    // Short low pulse to write a 1 bit
#define DELAY_WRITE_1_HIGH    55    // Recovery time after writing a 1 bit

#define DELAY_WRITE_0_LOW     65    // Long low pulse to write a 0 bit
#define DELAY_WRITE_0_HIGH     5    // Recovery time after writing a 0 bit

#define DELAY_READ_LOW         5    // Master pulls low to initiate a read slot
#define DELAY_READ_SAMPLE     10    // Time after releasing before sampling the line
#define DELAY_READ_HIGH       55    // Recovery time to complete the read slot

// ── Private helper functions ──────────────────────────────────────────────────

/** @brief Delay for the specified number of microseconds. */
static void delay_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

/** @brief Pull the 1-Wire bus low by switching the pin to output and driving 0. */
static void drive_low(void)
{
    gpio_set_direction(ONE_WIRE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ONE_WIRE_PIN, 0);
}

/** @brief Release the 1-Wire bus by switching the pin to input.
 *  The external 4.7kΩ pull-up resistor will pull the line back to 3.3V. */
static void release_line(void)
{
    gpio_set_direction(ONE_WIRE_PIN, GPIO_MODE_INPUT);
}

/** @brief Sample the current logic level on the 1-Wire bus.
 *  @return 1 if line is high, 0 if line is low */
static uint8_t read_line(void)
{
    return (uint8_t)gpio_get_level(ONE_WIRE_PIN);
}

/** @brief Write a single bit to the bus.
 *
 *  A 1 bit is written with a short low pulse followed by a long high.
 *  A 0 bit is written with a long low pulse followed by a short high.
 *  The sensor samples the line during the low pulse window.
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

/** @brief Read a single bit from the bus.
 *
 *  Master initiates the read slot with a short low pulse, then releases.
 *  The sensor holds the line low for a 0 or lets it float high for a 1.
 *  Master samples the line after DELAY_READ_SAMPLE microseconds.
 *
 *  @return 1 or 0
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

/** @brief Write a full byte to the bus, LSB first. */
static void DS18B20_WriteByte(uint8_t byte)
{
    for (int i = 0; i < 8; i++)
    {
        DS18B20_WriteBit(byte & 0x01);  // Send LSB
        byte >>= 1;                      // Shift next bit into position
    }
}

/** @brief Read a full byte from the bus, LSB first.
 *  @return The byte received from the sensor */
static uint8_t DS18B20_ReadByte(void)
{
    uint8_t data = 0;
    for (int i = 0; i < 8; i++)
    {
        data >>= 1;                         // Shift existing bits right
        if (DS18B20_ReadBit())
        {
            data |= 0x80;                   // Place new bit at MSB position
        }
    }
    return data;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool DS18B20_Reset(void)
{
    drive_low();                            // Pull bus low for reset pulse
    delay_us(DELAY_RESET_PULSE);
    release_line();                         // Release and wait for presence pulse
    delay_us(DELAY_PRESENCE_WAIT);
    uint8_t presence = !read_line();        // Sensor pulls low = present (invert to get true/false)
    delay_us(DELAY_PRESENCE_READ);          // Wait out the remainder of the reset window
    return presence;
}

bool DS18B20_Init(void)
{
    // Configure pin as input with internal pull-up as a backup
    // The external 4.7kΩ resistor is still required for reliable 1-Wire operation
    gpio_config_t cfg = 
    {
        .pin_bit_mask = (1ULL << ONE_WIRE_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    return DS18B20_Reset();     // Verify sensor is present before returning
}

void DS18B20_StartConversion(void)
{
    DS18B20_Reset();
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);    // Skip ROM addressing (single sensor on bus)
    DS18B20_WriteByte(DS18B20_CMD_CONVERT_T);   // Command sensor to begin measuring
    // Caller must wait at least 750ms for 12-bit conversion to complete
}

float DS18B20_ReadTemperature(void)
{
    DS18B20_Reset();
    DS18B20_WriteByte(DS18B20_CMD_SKIP_ROM);        // Skip ROM addressing
    DS18B20_WriteByte(DS18B20_CMD_READ_SCRATCHPAD); // Request scratchpad data

    uint8_t temp_lsb = DS18B20_ReadByte();          // Byte 0: temperature LSB
    uint8_t temp_msb = DS18B20_ReadByte();          // Byte 1: temperature MSB

    // Combine bytes into a signed 16-bit raw value
    // The DS18B20 returns temperature as a fixed-point number with 4 fractional bits
    int16_t temp_raw = (int16_t)((temp_msb << 8) | temp_lsb);

    // Convert raw value to Celsius: each LSB = 0.0625°C (1/16°C)
    return (float)temp_raw * 0.0625f;
}