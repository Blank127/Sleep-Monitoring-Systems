/**
 * @file OneWire_DS18B20_Sensor.h
 * @brief ESP-IDF driver for the DS18B20 temperature sensor over 1-Wire.
 *
 * Implements the 1-Wire protocol in software by bit-banging a single GPIO pin.
 * Requires a 4.7kΩ pull-up resistor from ONE_WIRE_PIN to 3.3V — the internal
 * ESP32 pull-up is not strong enough for reliable 1-Wire operation on its own.
 *
 * Typical usage:
 *   1. Call DS18B20_Init() once at startup to configure the GPIO and verify
 *      the sensor is present on the bus
 *   2. Call DS18B20_StartConversion() to trigger a temperature measurement
 *   3. Wait at least 750ms for the 12-bit conversion to complete
 *   4. Call DS18B20_ReadTemperature() to read the result in degrees Celsius
 *
 * @note Only a single DS18B20 is supported on the bus. The SKIP ROM command
 *       (0xCC) is used to address it directly without reading the 64-bit ROM code.
 */

#ifndef ONEWIRE_DS18B20_SENSOR_H
#define ONEWIRE_DS18B20_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

// ─────────────────────────────────────────────────────────────────────────────
// Pin configuration
// ─────────────────────────────────────────────────────────────────────────────

/** @def ONE_WIRE_PIN
 *  @brief GPIO pin used for 1-Wire communication with the DS18B20.
 *
 *  This pin is bit-banged in software to implement the 1-Wire protocol.
 *  A 4.7kΩ pull-up resistor to 3.3V is required on this pin.
 *  The internal ESP32 pull-up (~45kΩ) is insufficient for reliable operation.
 */
#define ONE_WIRE_PIN    GPIO_NUM_6

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Configure the 1-Wire GPIO pin and verify the sensor is present on the bus.
 *
 * Configures ONE_WIRE_PIN as an input with the internal pull-up enabled,
 * then issues a reset pulse and checks for a presence pulse from the DS18B20.
 *
 * Must be called once before any other DS18B20 function. If this returns
 * false, the sensor is not connected or the pull-up resistor is missing.
 *
 * @return true  if the sensor responded with a valid presence pulse.
 * @return false if no presence pulse was detected (sensor absent or wiring fault).
 */
bool DS18B20_Init(void);

/**
 * @brief Issue a 1-Wire reset pulse and check for a presence pulse.
 *
 * Pulls the bus low for 480µs, then releases it and samples after 70µs.
 * A connected DS18B20 will pull the bus low for 60–240µs to signal presence.
 *
 * Called internally before every command sequence. Can also be called
 * directly to verify sensor connectivity at runtime.
 *
 * @return true  if the sensor responded with a presence pulse.
 * @return false if the bus stayed high (no sensor detected).
 */
bool DS18B20_Reset(void);

/**
 * @brief Command the DS18B20 to begin a temperature conversion.
 *
 * Issues SKIP ROM (0xCC) followed by CONVERT T (0x44) to start a
 * 12-bit temperature measurement. The conversion takes up to 750ms
 * to complete — the caller must wait before calling DS18B20_ReadTemperature().
 *
 * @note Do not call DS18B20_ReadTemperature() until at least 750ms
 *       after this function returns, or the result will be stale.
 */
void DS18B20_StartConversion(void);

/**
 * @brief Read the most recent temperature conversion result from the sensor.
 *
 * Issues SKIP ROM (0xCC) followed by READ SCRATCHPAD (0xBE) and reads
 * the first two bytes (temperature LSB and MSB). Combines them into a
 * signed 16-bit fixed-point value and converts to Celsius.
 *
 * The DS18B20 stores temperature as a fixed-point value with 4 fractional
 * bits, so each LSB represents 0.0625°C (1/16°C).
 *
 * @note Must be called at least 750ms after DS18B20_StartConversion().
 *       Calling earlier will return the result of the previous conversion.
 *
 * @return Temperature in degrees Celsius as a float, with 0.0625°C resolution.
 *         Returns 0.0 if the conversion result is zero (unlikely in normal use).
 */
float DS18B20_ReadTemperature(void);

#endif // ONEWIRE_DS18B20_SENSOR_H