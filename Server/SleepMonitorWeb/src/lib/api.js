import axios from 'axios'

const api = axios.create({
  baseURL: import.meta.env.VITE_API_URL || '/api',
  headers: {
    'X-Api-Key': import.meta.env.VITE_API_KEY || '',
  }
})

export const getSessions      = () => api.get('/Sessions')
export const getSession       = (id) => api.get(`/Sessions/${id}`)
export const getReadings      = (sessionId) => api.get(`/Readings/session/${sessionId}`)
export const getLatestReading = () => api.get('/Readings/latest')
export const getStatus        = () => api.get('/Readings/status')