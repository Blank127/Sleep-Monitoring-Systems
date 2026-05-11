import { BrowserRouter, Routes, Route } from 'react-router-dom'
import LiveView from './pages/LiveView'
import SessionHistory from './pages/SessionHistory'
import SessionDetail from './pages/SessionDetail'
import Navbar from './components/Navbar'

function App() {
  return (
    <BrowserRouter>
      <div className="min-h-screen bg-gray-950 text-white">
        <Navbar />
        <main className="max-w-5xl mx-auto px-4 py-8">
          <Routes>
            <Route path="/" element={<LiveView />} />
            <Route path="/sessions" element={<SessionHistory />} />
            <Route path="/sessions/:id" element={<SessionDetail />} />
          </Routes>
        </main>
      </div>
    </BrowserRouter>
  )
}

export default App