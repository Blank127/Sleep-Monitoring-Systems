import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { getSessions } from '../lib/api'

function SessionHistory() {
  const [sessions, setSessions] = useState([])
  const [loading, setLoading]   = useState(true)

  useEffect(() => {
    getSessions()
      .then(res => setSessions(res.data))
      .finally(() => setLoading(false))
  }, [])

  const formatDate = (iso) =>
    new Date(iso).toLocaleString()

  const formatDuration = (minutes) => {
    if (minutes === null) return 'Active'
    if (minutes < 60) return `${Math.round(minutes)}m`
    return `${Math.floor(minutes / 60)}h ${Math.round(minutes % 60)}m`
  }

  return (
    <div>
      <h1 className="text-2xl font-bold mb-6">Session History</h1>

      {loading ? (
        <div className="text-center text-gray-400 py-16">Loading...</div>
      ) : sessions.length === 0 ? (
        <div className="bg-gray-900 border border-gray-800 rounded-xl p-8 text-center text-gray-400">
          No sessions recorded yet.
        </div>
      ) : (
        <div className="flex flex-col gap-3">
          {sessions.map(session => (
            <Link
              key={session.id}
              to={`/sessions/${session.id}`}
              className="bg-gray-900 border border-gray-800 rounded-xl p-5 hover:border-blue-600 transition-colors"
            >
              <div className="flex items-center justify-between">
                <div>
                  <p className="text-white font-medium">Session #{session.id}</p>
                  <p className="text-gray-400 text-sm mt-1">
                    {formatDate(session.startedAt)}
                  </p>
                </div>
                <div className="text-right">
                  <p className="text-blue-400 font-medium">
                    {formatDuration(session.duration)}
                  </p>
                  <p className="text-gray-400 text-sm mt-1">
                    {session.readingCount} readings
                  </p>
                </div>
              </div>
            </Link>
          ))}
        </div>
      )}
    </div>
  )
}

export default SessionHistory