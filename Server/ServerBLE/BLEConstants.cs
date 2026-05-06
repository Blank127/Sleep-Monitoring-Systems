namespace ServerBLE;

internal static class BleConstants
{
    // Must match BLE_DEVICE_NAME in BLE_Server.h
    public const string DeviceName = "SleepMonitor";

    // Must match SERVICE_UUID in BLE_Server.c
    public static readonly Guid ServiceUuid = Guid.Parse("12345678-1234-1234-1234-123456789abc");

    // Must match CHAR_UUID in BLE_Server.c (last byte 0xBD)
    public static readonly Guid CharacteristicUuid = Guid.Parse("12345678-1234-1234-1234-123456789abd");

    // Must match BLE_EOT_BYTE in BLE_Server.c
    public const byte EotByte = 0x04;
}