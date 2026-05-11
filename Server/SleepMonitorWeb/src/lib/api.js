import axios from 'axios'

const api = axios.create({
  baseURL: '/api',
})

export const getSessions      = () => api.get('/Sessions')
export const getSession       = (id) => api.get(`/Sessions/${id}`)
export const getReadings      = (sessionId) => api.get(`/Readings/session/${sessionId}`)
export const getLatestReading = () => api.get('/Readings/latest')
export const getStatus        = () => api.get('/Readings/status')