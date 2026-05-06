// ─────────────────────────────────────────────────────────────────────────────
// Sleep Monitor — BLE Console Server
//
// Entry point for the BLE console application. Connects to the ESP32 Sleep
// Monitor over BLE, reassembles chunked JSON payloads, and prints live
// readings to the console.
//
// Pipeline:
//   ESP32 (BLE notifications)
//     → BleClient     (scan, connect, receive raw chunks)
//     → PacketParser  (reassemble chunks → complete JSON)
//     → OnPacket      (parse fields, print to console)
//
// Press Ctrl+C to stop cleanly.
// ─────────────────────────────────────────────────────────────────────────────

using ServerBLE;

// ── Cancellation ──────────────────────────────────────────────────────────────
// CancellationTokenSource is the mechanism that stops the BLE loop cleanly.
// When Ctrl+C is pressed, e.Cancel = true prevents the process from terminating
// immediately, giving the loop time to unsubscribe and disconnect gracefully.
using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true; // Suppress the default "terminate process" behaviour
    cts.Cancel();    // Signal the BLE loop to stop
};

// ── Packet parser ─────────────────────────────────────────────────────────────
// PacketParser accumulates raw BLE chunks and fires OnPacket when a complete
// JSON message has been reassembled (signalled by the 0x04 EOT sentinel byte).
var parser = new PacketParser();

parser.OnPacket += (type, data) =>
{
    switch (type)
    {
        case "session_start":
            // The ESP32 sends this once on boot to signal a new session
            Console.WriteLine("\n[SESSION] New session started");
            break;

        case "reading":
            // Extract all fields from the parsed JSON element.
            // These property names must match the keys in build_reading_packet()
            // in the ESP32 firmware (payload.c).
            int hr = data.GetProperty("heart_rate").GetInt32();
            int br = data.GetProperty("breathe_rate").GetInt32();
            float temp = data.GetProperty("temperature_c").GetSingle();
            string zone = data.GetProperty("temp_zone").GetString() ?? "";
            int apnea = data.GetProperty("apnea_events").GetInt32();
            int dist = data.GetProperty("sleep_disturbance").GetInt32();

            Console.WriteLine($"\n[READING]");
            Console.WriteLine($"  Heart Rate:   {hr} BPM");
            Console.WriteLine($"  Breathe Rate: {br} BPM");
            Console.WriteLine($"  Temperature:  {temp:F1} °C [{zone}]");
            Console.WriteLine($"  Apnea Events: {apnea}");
            Console.WriteLine($"  Disturbance:  {dist}");
            break;

        default:
            // Unknown type — log it so firmware changes are visible immediately
            Console.WriteLine($"[WARN] Unknown packet type: {type}");
            break;
    }
};

// ── BLE client ────────────────────────────────────────────────────────────────
// BleClient handles the full connection lifecycle: scan → connect → subscribe
// → receive → disconnect → reconnect. Raw notification bytes are fed directly
// into the parser — BleClient has no knowledge of the packet format.
await using var client = new BleClient();

client.OnStatus += msg => Console.WriteLine($"[BLE] {msg}");
client.OnRawData += data => parser.Feed(data);

// Blocks here until Ctrl+C cancels the token
await client.RunAsync(cts.Token);