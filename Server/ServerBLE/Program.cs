using ServerBLE;

// Ctrl+C cancels the token which stops the BLE loop cleanly
using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cts.Cancel(); };

// PacketParser reassembles BLE chunks into complete JSON messages
var parser = new PacketParser();

// Fired when a complete packet has been parsed
parser.OnPacket += (type, data) =>
{
    switch (type)
    {
        case "session_start":
            Console.WriteLine("\n[SESSION] New session started");
            break;

        case "reading":
            // Extract each field from the JSON element
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
            Console.WriteLine($"[WARN] Unknown packet type: {type}");
            break;
    }
};

// BleClient handles scan → connect → subscribe → receive → reconnect
await using var client = new BleClient();

client.OnStatus += msg => Console.WriteLine($"[BLE] {msg}");
client.OnRawData += data => parser.Feed(data); // feed chunks into the parser

await client.RunAsync(cts.Token);