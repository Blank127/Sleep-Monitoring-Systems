using System;
using System.Collections.Generic;
using System.Text;

namespace ServerDB
{
    /// <summary>
    /// Represents one connected session from the ESP32 device.
    /// A new session is created each time a session_start packet is received.
    /// </summary>
    public class Session
    {
        /// <summary>
        /// Auto-incremented primary key.
        /// </summary>
        public int Id { get; set; }

        /// <summary>
        /// UTC timestamp when the session started.
        /// </summary>
        public DateTime StartedAt { get; set; } = DateTime.UtcNow;

        /// <summary>
        /// UTC timestamp when the session ended.
        /// Null if the session is still active.
        /// </summary>
        public DateTime? EndedAt { get; set; }

        /// <summary>
        /// All readings recorded during this session.
        /// </summary>
        public ICollection<Reading> Readings { get; set; } = [];
    }
}
