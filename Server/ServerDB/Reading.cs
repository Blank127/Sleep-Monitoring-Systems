using System;
using System.Collections.Generic;
using System.Text;

namespace ServerDB
{
    /// <summary>
    /// One sensor reading received from the ESP32 every 10 seconds.
    /// Maps directly to the JSON reading packet produced by payload.c.
    /// </summary>
    public class Reading
    {
        /// <summary>
        /// Auto-incremented primary key.
        /// </summary>
        public int Id { get; set; }

        /// <summary>
        /// Foreign key linking this reading to its parent session.
        /// </summary>
        public int SessionId { get; set; }

        /// <summary>
        /// Navigation property to the parent session.
        /// </summary>
        public Session Session { get; set; } = null!;

        /// <summary>
        /// UTC timestamp when this reading was received by the server.
        /// </summary>
        public DateTime RecordedAt { get; set; } = DateTime.UtcNow;

        /// <summary>
        /// Averaged heart rate in BPM over the 10s window.
        /// </summary>
        public int HeartRate { get; set; }

        /// <summary>
        /// Averaged breathing rate in BPM over the 10s window.
        /// </summary>
        public int BreatheRate { get; set; }

        /// <summary>
        /// Room temperature in degrees Celsius from the DS18B20.
        /// </summary>
        public float TemperatureC { get; set; }

        /// <summary>
        /// Temperature zone string e.g. "Ideal", "Too Hot".
        /// </summary>
        public string TempZone { get; set; } = string.Empty;

        /// <summary>
        /// Number of apnea events detected by the C1001.
        /// </summary>
        public int ApneaEvents { get; set; }

        /// <summary>
        /// Sleep disturbance code from the C1001.
        /// 0 = sleep less than 4hrs, 1 = sleep more than 12hrs,
        /// 2 = abnormal absence, 3 = none.
        /// </summary>
        public int SleepDisturbance { get; set; }
    }
}
