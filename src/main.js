import { createApp } from 'vue'
import { createPinia } from 'pinia'
import Vant from 'vant'
import 'vant/lib/index.css'

import './styles/index.css'

import App from './App.vue'
import router from './router'
import { setUnauthorizedHandler } from './api/http'

const app = createApp(App)

// pinia 必须先于 router：守卫里第一次 useAuthStore() 需要 active pinia
app.use(createPinia())
app.use(router)
// Vant 全量注册（产物 ~500KB，移动端首屏可接受；离线可用，无 CDN）
app.use(Vant)

/**
 * 401 后的跳转由这里注入（http.js 不 import router，避免循环依赖）
 * 与 admin 版完全同构，只是跳转目标是 H5 的 /login
 */
setUnauthorizedHandler(() => {
  const cur = router.currentRoute.value
  if (cur.name !== 'login') {
    router.replace({ name: 'login', query: { redirect: cur.fullPath } })
  }
})

app.mount('#app')
