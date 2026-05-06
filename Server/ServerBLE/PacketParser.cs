using System.Text;
using System.Text.Json;

namespace ServerBLE;

/// <summary>
/// Reassembles chunked BLE notification data into complete JSON messages.
/// </summary>
/// <remarks>
/// The ESP32 splits JSON payloads into MTU-sized chunks (default 20 bytes)
/// and sends a single <see cref="BleConstants.EotByte"/> (0x04) after the
/// last chunk to signal end-of-message.
///
/// Usage:
/// <code>
/// var parser = new PacketParser();
/// parser.OnPacket += (type, data) => Console.WriteLine(type);
/// client.OnRawData += data => parser.Feed(data);
/// </code>
///
/// When the BLE connection drops, call <see cref="Reset"/> to discard any
/// partially accumulated bytes before the next session begins.
/// </remarks>
internal sealed class PacketParser
{
    // ── Private state ─────────────────────────────────────────────────────────

    /// <summary>
    /// Accumulates raw bytes from incoming BLE chunks until the EOT sentinel arrives.
    /// </summary>
    private readonly List<byte> _buffer = [];

    // ── Public events ─────────────────────────────────────────────────────────

    /// <summary>
    /// Fired when a complete JSON message has been reassembled and parsed.
    /// </summary>
    /// <remarks>
    /// The first argument is the packet type string read from the JSON
    /// <c>"type"</c> field — either <c>"session_start"</c> or <c>"reading"</c>.
    /// The second argument is the full parsed <see cref="JsonElement"/>,
    /// cloned so it remains valid after the internal <see cref="JsonDocument"/>
    /// is disposed.
    /// </remarks>
    public event Action<string, JsonElement>? OnPacket;

    // ── Public API ────────────────────────────────────────────────────────────

    /// <summary>
    /// Feeds a raw BLE notification chunk into the parser.
    /// </summary>
    /// <remarks>
    /// Call this from the <see cref="BleClient.OnRawData"/> handler for every
    /// incoming chunk. When a single <see cref="BleConstants.EotByte"/> (0x04)
    /// is received the buffer is flushed, the JSON is parsed, and
    /// <see cref="OnPacket"/> is fired.
    /// </remarks>
    /// <param name="data">Raw bytes from a single BLE notification.</param>
    public void Feed(byte[] data)
    {
        // A single 0x04 byte signals end-of-message — flush and parse
        if (data.Length == 1 && data[0] == BleConstants.EotByte)
        {
            Flush();
            return;
        }

        // Still mid-message — accumulate this chunk and wait for more
        _buffer.AddRange(data);
    }

    /// <summary>
    /// Discards any partially accumulated bytes.
    /// </summary>
    /// <remarks>
    /// Call this when the BLE connection drops to prevent stale bytes from
    /// bleeding into the first message of the next session.
    /// </remarks>
    public void Reset() => _buffer.Clear();

    // ── Private methods ───────────────────────────────────────────────────────

    /// <summary>
    /// Joins all buffered bytes into a UTF-8 string, parses the JSON,
    /// and fires <see cref="OnPacket"/>.
    /// </summary>
    /// <remarks>
    /// Called automatically by <see cref="Feed"/> when the EOT sentinel arrives.
    /// If JSON parsing fails the error is logged and the parser resets silently —
    /// it does not throw, so the caller continues running and waits for the next message.
    /// </remarks>
    private void Flush()
    {
        if (_buffer.Count == 0) return;

        string json = Encoding.UTF8.GetString(_buffer.ToArray());

        // Clear immediately so the buffer is ready for the next message
        // even if parsing below throws
        _buffer.Clear();

        try
        {
            using var doc = JsonDocument.Parse(json);
            string type = doc.RootElement.GetProperty("type").GetString() ?? "unknown";

            // Clone the element before the using block disposes the JsonDocument.
            // Without Clone(), the JsonElement becomes invalid after disposal
            // and accessing it would throw an ObjectDisposedException.
            OnPacket?.Invoke(type, doc.RootElement.Clone());
        }
        catch (JsonException ex)
        {
            // Log and continue — a single corrupt packet should not crash the parser
            Console.WriteLine($"[Parser] Failed to parse JSON: {ex.Message}");
            Console.WriteLine($"[Parser] Raw: {json}");
        }
    }
}