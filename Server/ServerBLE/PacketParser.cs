using System.Text;
using System.Text.Json;

namespace ServerBLE;

internal sealed class PacketParser
{
    // Accumulates raw bytes from BLE chunks until the EOT sentinel arrives
    private readonly List<byte> _buffer = [];

    // Fired when a complete JSON message has been reassembled and parsed.
    // string  = packet type ("session_start" or "reading")
    // JsonElement = the full parsed JSON to read fields from
    public event Action<string, JsonElement>? OnPacket;

    /// <summary>
    /// Called for every incoming BLE notification chunk.
    /// Accumulates bytes until the EOT sentinel (0x04) signals end of message.
    /// </summary>
    public void Feed(byte[] data)
    {
        // A single 0x04 byte means the ESP32 has finished sending this message.
        // Flush the buffer to parse and fire OnPacket.
        if (data.Length == 1 && data[0] == BleConstants.EotByte)
        {
            Flush();
            return;
        }

        // Still mid-message — append this chunk and wait for more
        _buffer.AddRange(data);
    }

    /// <summary>
    /// Joins all buffered bytes into a UTF-8 string, parses the JSON,
    /// and fires OnPacket with the type and the full JSON element.
    /// Called automatically when the EOT sentinel arrives.
    /// </summary>
    private void Flush()
    {
        // Nothing accumulated — nothing to do
        if (_buffer.Count == 0) return;

        // Convert all buffered bytes into a UTF-8 JSON string
        string json = Encoding.UTF8.GetString(_buffer.ToArray());

        // Clear the buffer now so it's ready for the next message
        _buffer.Clear();

        try
        {
            // Parse the JSON and read the "type" field to identify the packet
            using var doc = JsonDocument.Parse(json);
            string type = doc.RootElement.GetProperty("type").GetString() ?? "unknown";

            // Clone the JsonElement before the doc is disposed (using var disposes
            // it at the end of this block, which would invalidate the element)
            OnPacket?.Invoke(type, doc.RootElement.Clone());
        }
        catch (JsonException ex)
        {
            // If parsing fails, log the error and the raw JSON so we can debug it.
            // Don't crash — just wait for the next message.
            Console.WriteLine($"[Parser] Failed to parse JSON: {ex.Message}");
            Console.WriteLine($"[Parser] Raw: {json}");
        }
    }

    /// <summary>
    /// Discards any partially accumulated bytes.
    /// Call this when the BLE connection drops so stale data
    /// doesn't bleed into the next session's first message.
    /// </summary>
    public void Reset() => _buffer.Clear();
}