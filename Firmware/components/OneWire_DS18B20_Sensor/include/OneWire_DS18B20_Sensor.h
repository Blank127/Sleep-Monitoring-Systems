/**
 * @file OneWire_DS18B20_Sensor.h
 * @brief ESP-IDF driver for the DS18B20 temperature sensor over 1-Wire.
 *
 * Implements the 1-Wire protocol in software using a single GPIO pin.
 * Requires a 4.7kΩ pull-up resistor from ONE_WIRE_PIN to 3.3V.
 *
 * Typical usage:
 *   1. Call DS18B20_Init() once at startup
 *   2. Call DS18B20_StartConversion() to trigger a measurement
 *   3. Wait at least 750ms for conversion to complete
 *   4. Call DS18B20_ReadTemperature() to get the result in Celsius
 */
#ifndef ONEWIRE_DS18B20_SENSOR_H
#define ONEWIRE_DS18B20_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

// --- Pin configuration ---
#define ONE_WIRE_PIN    GPIO_NUM_6  // Requires a 4.7kΩ pull-up resistor to 3.3V

// --- Public API ---

/** @brief Configure the GPIO pin and verify the sensor is present.
 *  @return true if sensor responded with a presence pulse, false otherwise */
bool DS18B20_Init(void);

/** @brief Send a reset pulse and check for a presence pulse from the sensor.
 *  @return true if sensor is present, false if no response */
bool DS18B20_Reset(void);

/** @brief Trigger a temperature conversion on the sensor.
 *  After calling this, wait at least 750ms before reading. */
void DS18B20_StartConversion(void);

/** @brief Read the temperature from the sensor scratchpad.
 *  @return Temperature in degrees Celsius as a float (resolution 0.0625°C) */
float DS18B20_ReadTemperature(void);

#endif // ONEWIRE_DS18B20_SENSOR_H