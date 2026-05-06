using ServerBLE;

using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) => { e.Cancel = true; cts.Cancel(); };

await using var client = new BleClient();

client.OnStatus += msg => Console.WriteLine($"[BLE] {msg}");
client.OnRawData += data => Console.WriteLine($"[RAW] {data.Length} bytes: {BitConverter.ToString(data)}");

await client.RunAsync(cts.Token);