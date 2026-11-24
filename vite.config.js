import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 后端地址（与 back/ReserveFlow 的 server.port 一致）
const API_TARGET = 'http://127.0.0.1:8080'

export default defineConfig({
  // 相对路径产物：既能挂到后端 /h5 下，也能放任意子目录
  base: './',
  plugins: [vue()],
  server: {
    // 5173 是 admin 开发服，H5 固定用 5174
    port: 5174,
    proxy: {
      '/api': { target: API_TARGET, changeOrigin: true },
    },
  },
  // 产物预览也带代理（dist 自己不会转发）
  preview: {
    port: 4174,
    proxy: {
      '/api': { target: API_TARGET, changeOrigin: true },
    },
  },
  build: {
    outDir: 'dist',
    // Vant 全量约 500KB，正常；超过 1MB 才需要怀疑引错了库
    chunkSizeWarningLimit: 1200,
    sourcemap: false,
  },
})
