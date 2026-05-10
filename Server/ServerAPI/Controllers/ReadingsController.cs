using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;
using ServerDB;

namespace ServerAPI.Controllers;

/// <summary>
/// Provides endpoints for retrieving sensor readings.
/// </summary>
[ApiController]
[Route("api/[controller]")]
public class ReadingsController : ControllerBase
{
    private readonly SleepMonitorDbContext _db;

    /// <summary>Initialises the controller with the database context.</summary>
    public ReadingsController(SleepMonitorDbContext db)
    {
        _db = db;
    }

    /// <summary>
    /// Returns all readings for a specific session ordered by time.
    /// </summary>
    [HttpGet("session/{sessionId}")]
    public async Task<IActionResult> GetReadingsForSession(int sessionId)
    {
        var readings = await _db.Readings
            .Where(r => r.SessionId == sessionId)
            .OrderBy(r => r.RecordedAt)
            .Select(r => new
            {
                r.Id,
                r.SessionId,
                r.RecordedAt,
                r.HeartRate,
                r.BreatheRate,
                r.TemperatureC,
                r.TempZone,
                r.ApneaEvents,
                r.SleepDisturbance
            })
            .ToListAsync();

        return Ok(readings);
    }

    /// <summary>
    /// Returns the most recent reading across all sessions.
    /// Used by the live view to show the current sensor state.
    /// </summary>
    [HttpGet("latest")]
    public async Task<IActionResult> GetLatestReading()
    {
        var reading = await _db.Readings
            .OrderByDescending(r => r.RecordedAt)
            .Select(r => new
            {
                r.Id,
                r.SessionId,
                r.RecordedAt,
                r.HeartRate,
                r.BreatheRate,
                r.TemperatureC,
                r.TempZone,
                r.ApneaEvents,
                r.SleepDisturbance
            })
            .FirstOrDefaultAsync();

        if (reading is null)
            return NotFound();

        return Ok(reading);
    }
}