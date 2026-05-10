using Microsoft.EntityFrameworkCore;
using System;
using System.Collections.Generic;
using System.Text;

namespace ServerDB
{
    /// <summary>
    /// EF Core database context for the Sleep Monitor.
    /// Manages the Sessions and Readings tables in PostgreSQL.
    /// </summary>
    public class SleepMonitorDbContext : DbContext
    {
        /// <summary>Initialises the context with the provided options.</summary>
        public SleepMonitorDbContext(DbContextOptions<SleepMonitorDbContext> options) : base(options) { }

        /// <summary>Table of all BLE sessions from the ESP32.</summary>
        public DbSet<Session> Sessions { get; set; }

        /// <summary>Table of all sensor readings received from the ESP32.</summary>
        public DbSet<Reading> Readings { get; set; }

        /// <inheritdoc/>
        protected override void OnModelCreating(ModelBuilder modelBuilder)
        {
            modelBuilder.Entity<Session>(e =>
            {
                e.HasKey(s => s.Id);
                e.Property(s => s.StartedAt).IsRequired();

                // One session has many readings — cascade delete removes
                // all readings when a session is deleted
                e.HasMany(s => s.Readings)
                 .WithOne(r => r.Session)
                 .HasForeignKey(r => r.SessionId)
                 .OnDelete(DeleteBehavior.Cascade);
            });

            modelBuilder.Entity<Reading>(e =>
            {
                e.HasKey(r => r.Id);
                e.Property(r => r.RecordedAt).IsRequired();
                e.Property(r => r.TempZone).HasMaxLength(32);

                // Indexes for the most common query patterns
                e.HasIndex(r => r.SessionId);
                e.HasIndex(r => r.RecordedAt);
            });
        }
    }
}
