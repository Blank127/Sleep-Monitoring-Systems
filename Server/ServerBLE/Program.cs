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
// appsettings.Local.json overrides it locally with the real password.
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

// ── Shared state ──────────────────────────────────────────────────────────────
// Tracks the current active session ID — null when no session is open
int? activeSessionId = null;

// ── Helper functions ──────────────────────────────────────────────────────────

// Closes the active session in the database by setting EndedAt.
// Safe to call multiple times — does nothing if no session is open.
async Task CloseActiveSessionAsync()
{
    if (activeSessionId is null) return;

    using var db = new SleepMonitorDbContext(dbOptions);
    var session = await db.Sessions.FindAsync(activeSessionId);
    if (session is not null)
    {
        session.EndedAt = DateTime.UtcNow;
        await db.SaveChangesAsync();
        Console.WriteLine($"[SESSION] Session {activeSessionId} closed.");
    }

    activeSessionId = null;
}

// ── Cancellation ──────────────────────────────────────────────────────────────
// CancellationTokenSource is the mechanism that stops the BLE loop cleanly.
// When Ctrl+C is pressed, e.Cancel = true prevents the process from terminating
// immediately, giving the loop time to close the session and disconnect gracefully.
using var cts = new CancellationTokenSource();
Console.CancelKeyPress += async (_, e) =>
{
    e.Cancel = true;
    await CloseActiveSessionAsync();
    try { cts.Cancel(); } catch (ObjectDisposedException) { }
};

// ── Packet parser ─────────────────────────────────────────────────────────────
// PacketParser accumulates raw BLE chunks and fires OnPacket when a complete
// JSON message has been reassembled (signalled by the 0x04 EOT sentinel byte).
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

        case "session_end":
            // Close the active session — person has left the sensor range
            await CloseActiveSessionAsync();
            Console.WriteLine("\n[SESSION] Session ended — person left");
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
// BleClient handles the full connection lifecycle: scan → connect → subscribe
// → receive → disconnect → reconnect. Raw notification bytes are fed directly
// into the parser.
await using var client = new BleClient();

client.OnStatus += msg => Console.WriteLine($"[BLE] {msg}");
client.OnRawData += data => parser.Feed(data);
client.OnDisconnected += async () => await CloseActiveSessionAsync();

// Blocks here until Ctrl+C cancels the token
await client.RunAsync(cts.Token);