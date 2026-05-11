import { useEffect, useState } from 'react'
import { getLatestReading, getStatus } from '../lib/api'

function StatCard({ label, value, unit }) {
  return (
    <div className="bg-gray-900 rounded-xl p-5 border border-gray-800">
      <p className="text-gray-400 text-sm mb-1">{label}</p>
      <p className="text-3xl font-semibold text-white">
        {value}
        {unit && <span className="text-lg text-gray-400 ml-1">{unit}</span>}
      </p>
    </div>
  )
}

function StatusBadge({ connected }) {
  return (
    <div className={`flex items-center gap-2 px-3 py-1 rounded-full text-sm font-medium border ${
      connected
        ? 'bg-green-950 border-green-800 text-green-400'
        : 'bg-gray-900 border-gray-700 text-gray-400'
    }`}>
      <span className={`w-2 h-2 rounded-full ${
        connected ? 'bg-green-400 animate-pulse' : 'bg-gray-500'
      }`} />
      {connected ? 'Connected' : 'Disconnected'}
    </div>
  )
}

function LiveView() {
  const [reading, setReading]     = useState(null)
  const [connected, setConnected] = useState(false)
  const [error, setError]         = useState(null)

  const fetchData = async () => {
    try {
      const [readingRes, statusRes] = await Promise.all([
        getLatestReading(),
        getStatus()
      ])
      setReading(readingRes.data)
      setConnected(statusRes.data.connected)
      setError(null)
    } catch {
      setError('No readings available')
      setConnected(false)
    }
  }

  // Poll every 10 seconds to match the ESP32 send rate
  useEffect(() => {
    fetchData()
    const interval = setInterval(fetchData, 10_000)
    return () => clearInterval(interval)
  }, [])

  const disturbanceLabel = (code) => {
    switch (code) {
      case 0: return 'Sleep < 4 hrs'
      case 1: return 'Sleep > 12 hrs'
      case 2: return 'Abnormal absence'
      case 3: return 'None'
      default: return 'Unknown'
    }
  }

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <div className="flex items-center gap-4">
          <h1 className="text-2xl font-bold">Live View</h1>
          <StatusBadge connected={connected} />
        </div>
        {reading && (
          <span className="text-xs text-gray-400">
            Last updated: {new Date(reading.recordedAt).toLocaleTimeString()}
          </span>
        )}
      </div>

      {error ? (
        <div className="bg-gray-900 border border-gray-800 rounded-xl p-8 text-center text-gray-400">
          {error}
        </div>
      ) : !reading ? (
        <div className="text-center text-gray-400 py-16">Loading...</div>
      ) : (
        <div className="grid grid-cols-2 md:grid-cols-3 gap-4">
          <StatCard label="Heart Rate"   value={reading.heartRate}   unit="BPM" />
          <StatCard label="Breathe Rate" value={reading.breatheRate} unit="BPM" />
          <StatCard label="Temperature"  value={`${reading.temperatureC.toFixed(1)}`} unit="°C" />
          <StatCard label="Temp Zone"    value={reading.tempZone} />
          <StatCard label="Apnea Events" value={reading.apneaEvents} />
          <StatCard label="Disturbance"  value={disturbanceLabel(reading.sleepDisturbance)} />
        </div>
      )}
    </div>
  )
}

export default LiveView