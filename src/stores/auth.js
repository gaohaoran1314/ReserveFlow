// =============================================================================
// src/stores/auth.js  —  登录态（token + user，落 localStorage）
// 对应 ARCH-P4 §4
//
// 三条要点：
//   1) restore() 幂等，只跑一次；**不在守卫里校验 token 有效性**（不调 /auth/me），
//      过期令牌由 http.js 的 401 拦截器统一踢出（少一次请求、后端故障时也不会误踢）；
//   2) login() 成功后同时写内存与 localStorage，F5 刷新不掉登录；
//   3) logout() 即使接口失败也必须清本地（否则会卡在"看着已登录但什么都 401"）。
// =============================================================================
import { defineStore } from 'pinia'

import * as authApi from '../api/auth'
import { TOKEN_KEY, USER_KEY } from '../constants/storage'

export const useAuthStore = defineStore('auth', {
  state: () => ({
    token: '',
    user: null,
    role: '',
    restored: false,
  }),

  getters: {
    isLoggedIn: (s) => !!s.token,
    isAdmin: (s) => s.role === 'ADMIN',
    displayName: (s) => s.user?.username || '',
    creditScore: (s) => s.user?.creditScore ?? null,
  },

  actions: {
    /** 从 localStorage 恢复（刷新用）。幂等：restored=true 后直接返回。 */
    restore() {
      if (this.restored) return
      const token = localStorage.getItem(TOKEN_KEY)
      const rawUser = localStorage.getItem(USER_KEY)
      if (token) this.token = token
      if (rawUser) {
        try {
          this.user = JSON.parse(rawUser)
        } catch {
          // 脏数据直接丢弃，不影响启动
          this.user = null
        }
      }
      this.role = this.user?.role || ''
      this.restored = true
    },

    /**
     * 登录
     * @param {string} username
     * @param {string} password
     * @returns {Promise<object>} 登录响应的 data（含 token / user / expiresAt）
     */
    async login(username, password) {
      const data = await authApi.login({ username, password })
      this.token = data?.token || ''
      this.user = data?.user || null
      this.role = this.user?.role || ''
      this.restored = true

      if (this.token) localStorage.setItem(TOKEN_KEY, this.token)
      if (this.user) localStorage.setItem(USER_KEY, JSON.stringify(this.user))
      return data
    },

    /** 登出：先尽力通知后端，再无条件清本地 */
    async logout() {
      try {
        await authApi.logout()
      } catch {
        // 后端不可用 / 令牌已失效都不影响本地登出
      }
      this.clear()
    },

    /** 只清本地（401 拦截器也会直接调这个语义） */
    clear() {
      this.token = ''
      this.user = null
      this.role = ''
      this.restored = true
      localStorage.removeItem(TOKEN_KEY)
      localStorage.removeItem(USER_KEY)
    },

    /** 主动刷新一次用户信息（顶栏"刷新"用，可选） */
    async fetchMe() {
      const me = await authApi.getMe()
      if (me) {
        this.user = me
        this.role = me.role || this.role
        localStorage.setItem(USER_KEY, JSON.stringify(me))
      }
      return me
    },
  },
})
