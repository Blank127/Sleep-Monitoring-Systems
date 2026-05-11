import { useEffect, useState } from 'react'
import { useParams, Link } from 'react-router-dom'
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid,
  Tooltip, ResponsiveContainer, Legend
} from 'recharts'
import { getSession, getReadings } from '../lib/api'

function SessionDetail() {
  const { id }                  = useParams()
  const [session, setSession]   = useState(null)
  const [readings, setReadings] = useState([])
  const [loading, setLoading]   = useState(true)

  useEffect(() => {
    Promise.all([getSession(id), getReadings(id)])
      .then(([sessionRes, readingsRes]) => {
        setSession(sessionRes.data)
        setReadings(readingsRes.data.map(r => ({
          ...r,
          time: new Date(r.recordedAt).toLocaleTimeString(),
        })))
      })
      .finally(() => setLoading(false))
  }, [id])

  if (loading) return (
    <div className="text-center text-gray-400 py-16">Loading...</div>
  )

  if (!session) return (
    <div className="text-center text-gray-400 py-16">Session not found.</div>
  )

  return (
    <div>
      <div className="flex items-center gap-4 mb-6">
        <Link to="/sessions" className="text-gray-400 hover:text-white text-sm">
          ← Back
        </Link>
        <h1 className="text-2xl font-bold">Session #{session.id}</h1>
      </div>

      {/* Session summary */}
      <div className="grid grid-cols-2 md:grid-cols-3 gap-4 mb-8">
        <div className="bg-gray-900 border border-gray-800 rounded-xl p-4">
          <p className="text-gray-400 text-sm">Started</p>
          <p className="text-white font-medium mt-1">
            {new Date(session.startedAt).toLocaleString()}
          </p>
        </div>
        <div className="bg-gray-900 border border-gray-800 rounded-xl p-4">
          <p className="text-gray-400 text-sm">Duration</p>
          <p className="text-white font-medium mt-1">
            {session.duration
              ? `${Math.round(session.duration)}m`
              : 'Active'}
          </p>
        </div>
        <div className="bg-gray-900 border border-gray-800 rounded-xl p-4">
          <p className="text-gray-400 text-sm">Readings</p>
          <p className="text-white font-medium mt-1">{session.readingCount}</p>
        </div>
      </div>

      {readings.length === 0 ? (
        <div className="bg-gray-900 border border-gray-800 rounded-xl p-8 text-center text-gray-400">
          No readings for this session.
        </div>
      ) : (
        <div className="flex flex-col gap-6">
          {/* Heart rate chart */}
          <div className="bg-gray-900 border border-gray-800 rounded-xl p-5">
            <h2 className="text-white font-semibold mb-4">Heart Rate</h2>
            <ResponsiveContainer width="100%" height={200}>
              <LineChart data={readings}>
                <CartesianGrid strokeDasharray="3 3" stroke="#1f2937" />
                <XAxis dataKey="time" tick={{ fill: '#9ca3af', fontSize: 11 }} />
                <YAxis tick={{ fill: '#9ca3af', fontSize: 11 }} />
                <Tooltip contentStyle={{ backgroundColor: '#111827', border: '1px solid #374151' }} />
                <Line type="monotone" dataKey="heartRate" stroke="#3b82f6" dot={false} strokeWidth={2} />
              </LineChart>
            </ResponsiveContainer>
          </div>

          {/* Breathe rate chart */}
          <div className="bg-gray-900 border border-gray-800 rounded-xl p-5">
            <h2 className="text-white font-semibold mb-4">Breathing Rate</h2>
            <ResponsiveContainer width="100%" height={200}>
              <LineChart data={readings}>
                <CartesianGrid strokeDasharray="3 3" stroke="#1f2937" />
                <XAxis dataKey="time" tick={{ fill: '#9ca3af', fontSize: 11 }} />
                <YAxis tick={{ fill: '#9ca3af', fontSize: 11 }} />
                <Tooltip contentStyle={{ backgroundColor: '#111827', border: '1px solid #374151' }} />
                <Line type="monotone" dataKey="breatheRate" stroke="#10b981" dot={false} strokeWidth={2} />
              </LineChart>
            </ResponsiveContainer>
          </div>

          {/* Temperature chart */}
          <div className="bg-gray-900 border border-gray-800 rounded-xl p-5">
            <h2 className="text-white font-semibold mb-4">Temperature</h2>
            <ResponsiveContainer width="100%" height={200}>
              <LineChart data={readings}>
                <CartesianGrid strokeDasharray="3 3" stroke="#1f2937" />
                <XAxis dataKey="time" tick={{ fill: '#9ca3af', fontSize: 11 }} />
                <YAxis tick={{ fill: '#9ca3af', fontSize: 11 }} />
                <Tooltip contentStyle={{ backgroundColor: '#111827', border: '1px solid #374151' }} />
                <Line type="monotone" dataKey="temperatureC" stroke="#f59e0b" dot={false} strokeWidth={2} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
      )}
    </div>
  )
}

export default SessionDetail