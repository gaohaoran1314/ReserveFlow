// =============================================================================
// src/router/guard.js  —  全局前置守卫（ARCH-P4 §2.2）
//
// 三条规则：
//   1) 首次进入先从 localStorage 恢复登录态（F5 不掉登录）
//   2) 公开页（登录/404）：已登录访问 /login 自动去 /dashboard
//   3) 受保护页：无 token → /login?redirect=<原路径>；非 ADMIN → 提示后回登录页
//
// 刻意**不在守卫里校验 token 是否过期**（不调 /auth/me）：过期令牌由
// http.js 的 401 拦截器统一踢出，省一次请求，也避免后端抖动时把人误踢。
// =============================================================================
import { ElMessage } from 'element-plus'

import { useAuthStore } from '../stores/auth'
import { LOCAL_TIP } from '../constants/errorCode'

export function setupGuard(router) {
  router.beforeEach((to) => {
    const auth = useAuthStore()

    // 1) 刷新后首次进入：恢复本地登录态
    if (!auth.restored) auth.restore()

    // 2) 公开页
    if (to.meta?.public) {
      if (to.name === 'login' && auth.token) {
        return { name: 'dashboard' }
      }
      return true
    }

    // 3) 未登录 → 登录页，并记住原路径
    if (!auth.token) {
      return { name: 'login', query: { redirect: to.fullPath } }
    }

    // 4) 非管理员：提示「不是管理员」而不是「密码错误」，避免误导
    if (auth.role !== 'ADMIN') {
      ElMessage.error(LOCAL_TIP.notAdmin)
      auth.clear()
      return { name: 'login' }
    }

    return true
  })
}
