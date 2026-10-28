import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

// dev:  ARM=http://192.168.1.50 npm run dev   (or leave default xarm.local)
// build: one self-contained index.html -> gzipped into firmware/main/www/
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  server: { proxy: { '/api': process.env.ARM || 'http://xarm.local' } },
  build: { outDir: 'dist', assetsInlineLimit: 100000000, target: ['es2019', 'safari13'], cssTarget: 'safari13' },   // older iPhones too
})
