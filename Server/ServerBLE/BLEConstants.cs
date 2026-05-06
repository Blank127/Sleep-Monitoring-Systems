namespace ServerBLE;

/// <summary>
/// BLE protocol constants shared between the C# server and the ESP32 firmware.
/// </summary>
/// <remarks>
/// Every value in this class must exactly match its counterpart in the ESP32
/// firmware. If any value is changed here it must be updated in the firmware too,
/// and vice versa.
///
/// Firmware cross-references:
/// <list type="bullet">
///   <item><see cref="DeviceName"/>         — BLE_DEVICE_NAME  in BLE_Server.h</item>
///   <item><see cref="ServiceUuid"/>         — SERVICE_UUID     in BLE_Server.c</item>
///   <item><see cref="CharacteristicUuid"/>  — CHAR_UUID        in BLE_Server.c</item>
///   <item><see cref="EotByte"/>             — BLE_EOT_BYTE     in BLE_Server.c</item>
/// </list>
/// </remarks>
internal static class BleConstants
{
    /// <summary>
    /// Advertised BLE device name used to identify the ESP32 during scanning.
    /// </summary>
    /// <remarks>
    /// Must match <c>BLE_DEVICE_NAME</c> defined in <c>BLE_Server.h</c>.
    /// The <see cref="BleClient"/> scans for this name to find the device.
    /// </remarks>
    public const string DeviceName = "SleepMonitor";

    /// <summary>
    /// Primary GATT service UUID: <c>12345678-1234-1234-1234-123456789ABC</c>
    /// </summary>
    /// <remarks>
    /// Must match <c>SERVICE_UUID</c> defined in <c>BLE_Server.c</c>.
    /// NimBLE stores 128-bit UUIDs in little-endian byte order, so the bytes
    /// in <c>BLE_UUID128_INIT()</c> appear in reverse compared to this string.
    /// </remarks>
    public static readonly Guid ServiceUuid =
        Guid.Parse("12345678-1234-1234-1234-123456789abc");

    /// <summary>
    /// Notify characteristic UUID: <c>12345678-1234-1234-1234-123456789ABD</c>
    /// </summary>
    /// <remarks>
    /// Must match <c>CHAR_UUID</c> defined in <c>BLE_Server.c</c>.
    /// Identical to <see cref="ServiceUuid"/> except the last byte is
    /// <c>0xBD</c> instead of <c>0xBC</c>.
    /// The ESP32 pushes JSON chunks to the client via notifications on this characteristic.
    /// </remarks>
    public static readonly Guid CharacteristicUuid =
        Guid.Parse("12345678-1234-1234-1234-123456789abd");

    /// <summary>
    /// End-of-transmission sentinel byte (<c>0x04</c>).
    /// </summary>
    /// <remarks>
    /// Must match <c>BLE_EOT_BYTE</c> defined in <c>BLE_Server.c</c>.
    /// After sending the last JSON chunk the ESP32 sends a single byte with
    /// this value to signal end-of-message. The <see cref="PacketParser"/>
    /// uses this to know when to flush its buffer and parse the accumulated JSON.
    /// The value <c>0x04</c> follows the ASCII EOT (End of Transmission) convention.
    /// </remarks>
    public const byte EotByte = 0x04;
}