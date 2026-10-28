import { defineConfig } from 'vite'

// served from https://jobot101.github.io/xarm/ — relative base so it works under /xarm/
export default defineConfig({ base: './', build: { outDir: 'dist', target: 'es2022' } })
