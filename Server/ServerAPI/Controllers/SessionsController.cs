using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;
using ServerDB;

namespace ServerAPI.Controllers;

/// <summary>
/// Provides endpoints for retrieving sleep session data.
/// </summary>
[ApiController]
[Route("api/[controller]")]
public class SessionsController : ControllerBase
{
    private readonly SleepMonitorDbContext _db;

    /// <summary>Initialises the controller with the database context.</summary>
    public SessionsController(SleepMonitorDbContext db)
    {
        _db = db;
    }

    /// <summary>
    /// Returns all sessions ordered by most recent first.
    /// </summary>
    [HttpGet]
    public async Task<IActionResult> GetSessions()
    {
        var sessions = await _db.Sessions
            .OrderByDescending(s => s.StartedAt)
            .Select(s => new
            {
                s.Id,
                s.StartedAt,
                s.EndedAt,
                Duration = s.EndedAt.HasValue
                    ? (s.EndedAt.Value - s.StartedAt).TotalMinutes
                    : (double?)null,
                ReadingCount = s.Readings.Count
            })
            .ToListAsync();

        return Ok(sessions);
    }

    /// <summary>
    /// Returns a single session by ID.
    /// </summary>
    [HttpGet("{id}")]
    public async Task<IActionResult> GetSession(int id)
    {
        var session = await _db.Sessions
            .Where(s => s.Id == id)
            .Select(s => new
            {
                s.Id,
                s.StartedAt,
                s.EndedAt,
                Duration = s.EndedAt.HasValue
                    ? (s.EndedAt.Value - s.StartedAt).TotalMinutes
                    : (double?)null,
                ReadingCount = s.Readings.Count
            })
            .FirstOrDefaultAsync();

        if (session is null)
            return NotFound();

        return Ok(session);
    }
}