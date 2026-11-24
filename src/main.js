import { createApp } from 'vue'
import { createPinia } from 'pinia'
import ElementPlus from 'element-plus'
import zhCn from 'element-plus/es/locale/lang/zh-cn'
import * as ElementPlusIconsVue from '@element-plus/icons-vue'

import 'element-plus/dist/index.css'
import './styles/index.css'

import App from './App.vue'
import router from './router'

const app = createApp(App)

// 图标全量注册（离线可用：全部来自依赖包内的 SVG，无 CDN）
for (const [name, comp] of Object.entries(ElementPlusIconsVue)) {
  app.component(name, comp)
}

// 顺序很重要：pinia 必须先于 router 安装，
// 否则 router 守卫里第一次 useAuthStore() 会拿不到 active pinia 而报错
app.use(createPinia())
app.use(router)
app.use(ElementPlus, { locale: zhCn })

app.mount('#app')
