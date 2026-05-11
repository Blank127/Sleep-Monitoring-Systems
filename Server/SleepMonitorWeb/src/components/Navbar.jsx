import { Link, useLocation } from 'react-router-dom'

function Navbar() {
  const location = useLocation()

  const linkClass = (path) =>
    `px-4 py-2 rounded-lg text-sm font-medium transition-colors ${
      location.pathname === path
        ? 'bg-blue-600 text-white'
        : 'text-gray-400 hover:text-white hover:bg-gray-800'
    }`

  return (
    <nav className="border-b border-gray-800 bg-gray-900">
      <div className="max-w-5xl mx-auto px-4 py-3 flex items-center justify-between">
        <span className="text-lg font-semibold text-white">
          Sleep Monitor
        </span>
        <div className="flex gap-2">
          <Link to="/" className={linkClass('/')}>Live</Link>
          <Link to="/sessions" className={linkClass('/sessions')}>Sessions</Link>
        </div>
      </div>
    </nav>
  )
}

export default Navbar