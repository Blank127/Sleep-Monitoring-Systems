// ─────────────────────────────────────────────────────────────────────────────
// Sleep Monitor — BLE Console Server
//
// Entry point for the BLE console application. Connects to the ESP32 Sleep
// Monitor over BLE, reassembles chunked JSON payloads, and saves readings
// to PostgreSQL.
//
// Pipeline:
//   ESP32 (BLE notifications)
//     → BleClient     (scan, connect, receive raw chunks)
//     → PacketParser  (reassemble chunks → complete JSON)
//     → OnPacket      (parse fields, save to database, print to console)
//
// Press Ctrl+C to stop cleanly.
// ─────────────────────────────────────────────────────────────────────────────

using Microsoft.EntityFrameworkCore;
using Microsoft.Extensions.Configuration;
using ServerBLE;
using ServerDB;

// ── Configuration ─────────────────────────────────────────────────────────────
// Load connection string from appsettings.json.
var config = new ConfigurationBuilder()
    .AddJsonFile("appsettings.json")
    .AddJsonFile("appsettings.Local.json", optional: true)
    .Build();

string connectionString = config.GetConnectionString("DefaultConnection")!;

// ── Database ──────────────────────────────────────────────────────────────────
// Build EF Core options and create the tables if they don't exist yet
var dbOptions = new DbContextOptionsBuilder<SleepMonitorDbContext>()
    .UseNpgsql(connectionString)
    .Options;

using (var db = new SleepMonitorDbContext(dbOptions))
{
    db.Database.EnsureCreated();
    Console.WriteLine("[DB] Database ready.");
}

// Tracks the current active session ID
int? activeSessionId = null;

// ── Cancellation ──────────────────────────────────────────────────────────────
using var cts = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    cts.Cancel();
};

// ── Packet parser ─────────────────────────────────────────────────────────────
var parser = new PacketParser();

parser.OnPacket += async (type, data) =>
{
    switch (type)
    {
        case "session_start":
            // Create a new session record in the database
            using (var db = new SleepMonitorDbContext(dbOptions))
            {
                var session = new Session { StartedAt = DateTime.UtcNow };
                db.Sessions.Add(session);
                await db.SaveChangesAsync();
                activeSessionId = session.Id;
                Console.WriteLine($"\n[SESSION] New session started — id={session.Id}");
            }
            break;

        case "reading":
            // Auto-create a session if we missed the session_start packet
            if (activeSessionId is null)
            {
                using var db = new SleepMonitorDbContext(dbOptions);
                var session = new Session { StartedAt = DateTime.UtcNow };
                db.Sessions.Add(session);
                await db.SaveChangesAsync();
                activeSessionId = session.Id;
            }

            // Save the reading to the database
            using (var dbReading = new SleepMonitorDbContext(dbOptions))
            {
                var reading = new Reading
                {
                    SessionId = activeSessionId!.Value,
                    RecordedAt = DateTime.UtcNow,
                    HeartRate = data.GetProperty("heart_rate").GetInt32(),
                    BreatheRate = data.GetProperty("breathe_rate").GetInt32(),
                    TemperatureC = data.GetProperty("temperature_c").GetSingle(),
                    TempZone = data.GetProperty("temp_zone").GetString() ?? "",
                    ApneaEvents = data.GetProperty("apnea_events").GetInt32(),
                    SleepDisturbance = data.GetProperty("sleep_disturbance").GetInt32(),
                };

                dbReading.Readings.Add(reading);
                await dbReading.SaveChangesAsync();
            }

            // Print to console
            int hr = data.GetProperty("heart_rate").GetInt32();
            int br = data.GetProperty("breathe_rate").GetInt32();
            float temp = data.GetProperty("temperature_c").GetSingle();
            string zone = data.GetProperty("temp_zone").GetString() ?? "";
            int apnea = data.GetProperty("apnea_events").GetInt32();
            int dist = data.GetProperty("sleep_disturbance").GetInt32();

            Console.WriteLine($"\n[READING] saved to database");
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

// ── BLE client ────────────────────────────────────────────────────────────────
await using var client = new BleClient();

client.OnStatus += msg => Console.WriteLine($"[BLE] {msg}");
client.OnRawData += data => parser.Feed(data);

await client.RunAsync(cts.Token);