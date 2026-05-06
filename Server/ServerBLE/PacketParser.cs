using System.Text;
using System.Text.Json;

namespace ServerBLE;

internal sealed class PacketParser
{
    private readonly List<byte> _buffer = [];

    public event Action<string, JsonElement>? OnPacket;

    public void Feed(byte[] data)
    {
        // EOT sentinel — message is complete, flush the buffer
        if (data.Length == 1 && data[0] == BleConstants.EotByte)
        {
            Flush();
            return;
        }

        _buffer.AddRange(data);
    }

    private void Flush()
    {
        if (_buffer.Count == 0) return;

        string json = Encoding.UTF8.GetString(_buffer.ToArray());
        _buffer.Clear();

        try
        {
            using var doc = JsonDocument.Parse(json);
            string type = doc.RootElement.GetProperty("type").GetString() ?? "unknown";
            OnPacket?.Invoke(type, doc.RootElement.Clone());
        }
        catch (JsonException ex)
        {
            Console.WriteLine($"[Parser] Failed to parse JSON: {ex.Message}");
            Console.WriteLine($"[Parser] Raw: {json}");
        }
    }

    public void Reset() => _buffer.Clear();
}