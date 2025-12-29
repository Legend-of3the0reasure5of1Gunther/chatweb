import { defineConfig, loadEnv } from 'vite'
import vue from '@vitejs/plugin-vue'
import { resolve } from 'path'

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '')
  
  return {
    plugins: [vue()],
    
    resolve: {
      alias: {
        '@': resolve(__dirname, './src'),
        '@components': resolve(__dirname, './src/components'),
        '@views': resolve(__dirname, './src/views'),
        '@utils': resolve(__dirname, './src/utils'),
        '@stores': resolve(__dirname, './src/stores'),
        '@types': resolve(__dirname, './src/types'),
      },
    },
    
    server: {
      host: '0.0.0.0',
      port: 3000,
      strictPort: true,
      https: env.VITE_SSL_ENABLED === 'true' ? {
        key: env.VITE_SSL_KEY_PATH,
        cert: env.VITE_SSL_CERT_PATH,
      } : false,
      
      proxy: {
        '/api': {
          target: env.VITE_API_URL || 'http://localhost:8080',
          changeOrigin: true,
          secure: false,
          ws: true,
        },
        '/ws': {
          target: env.VITE_WS_URL || 'ws://localhost:8081',
          changeOrigin: true,
          ws: true,
          secure: false,
        },
      },
    },
    
    build: {
      outDir: 'dist',
      assetsDir: 'assets',
      sourcemap: mode === 'production' ? 'hidden' : true,
      minify: 'terser',
      terserOptions: {
        compress: {
          drop_console: mode === 'production',
          drop_debugger: mode === 'production',
        },
      },
      
      rollupOptions: {
        output: {
          manualChunks: {
            vendor: ['vue', 'vue-router', 'pinia'],
            utils: ['axios', 'dayjs', 'lodash-es'],
            security: ['crypto-js', 'jsencrypt'],
            ui: ['element-plus', '@element-plus/icons-vue'],
          },
          
          entryFileNames: 'assets/[name]-[hash].js',
          chunkFileNames: 'assets/[name]-[hash].js',
          assetFileNames: 'assets/[name]-[hash].[ext]',
        },
      },
    },
    
    css: {
      preprocessorOptions: {
        scss: {
          additionalData: `@import "@/styles/variables.scss";`,
        },
      },
    },
    
    define: {
      __APP_VERSION__: JSON.stringify(env.npm_package_version || '1.0.0'),
      __BUILD_TIMESTAMP__: JSON.stringify(new Date().toISOString()),
    },
  }
})