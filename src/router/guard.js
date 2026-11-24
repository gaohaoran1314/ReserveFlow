// =============================================================================
// src/router/guard.js  —  前置守卫（ARCH-P5 §2.2）
//
// 与 P4 同构，但**不校验 ADMIN**：学生端只要求"已登录"，
// 管理员用 H5 看数据也允许（产品没要求区分）。
// 额外拦 DISABLED：禁用账号连登录页都进不去后续页面（登录成功也会被弹回）。
// =============================================================================
import { showToast } from 'vant'

import { useAuthStore } from '../stores/auth'
import { LOCAL_TIP } from '../constants/errorCode'

export function setupGuard(router) {
  router.beforeEach((to) => {
    const auth = useAuthStore()

    // 1) 首次进入：从 localStorage 恢复（F5 不掉登录）
    if (!auth.restored) auth.restore()

    // 2) 公开页
    if (to.meta?.public) {
      if (to.name === 'login' && auth.token) return { name: 'home' }
      return true
    }

    // 3) 未登录 → 登录页并记住原路径
    if (!auth.token) {
      return { name: 'login', query: { redirect: to.fullPath } }
    }

    // 4) 禁用账号：清登录态并拒绝
    if (auth.isDisabled) {
      showToast(LOCAL_TIP.disabled)
      auth.clear()
      return { name: 'login' }
    }

    return true
  })
}
