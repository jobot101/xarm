import { createRoot } from 'react-dom/client'
import App from './App.jsx'
import './styles.css'

const start = () => createRoot(document.getElementById('root')).render(<App />)
// preview build: fake firmware in the page (src/preview.js); the firmware build drops this branch
if (import.meta.env.VITE_PREVIEW) import('./preview.js').then(start)
else start()
