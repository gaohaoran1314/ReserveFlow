// =============================================================================
// src/stores/auth.js  —  登录态（与 admin 版同构，只换 storage key）
//
// 要点：
//   1) restore() 幂等，只跑一次；不在守卫里调 /auth/me（过期令牌由 401 拦截器踢出）
//   2) login() 成功后同时写内存与 localStorage，F5 不掉登录
//   3) logout() 即使接口失败也清本地
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
    /** 上次成功拉到 /auth/me 的时刻（**内存变量，不持久化**）：refreshIfStale 的节流依据 */
    lastFetchAt: 0,
  }),

  getters: {
    isLoggedIn: (s) => !!s.token,
    displayName: (s) => s.user?.realName || s.user?.username || '',
    username: (s) => s.user?.username || '',
    creditScore: (s) => s.user?.creditScore ?? null,
    /** 黑名单：首页挂 notice-bar 用（主理人决策 3：不用弹窗） */
    isBlacklisted: (s) => s.user?.status === 'BLACKLIST',
    /** 禁用账号：守卫直接拦 */
    isDisabled: (s) => s.user?.status === 'DISABLED',
    status: (s) => s.user?.status || '',
  },

  actions: {
    /** 从 localStorage 恢复（刷新用）。幂等。 */
    restore() {
      if (this.restored) return
      const token = localStorage.getItem(TOKEN_KEY)
      const rawUser = localStorage.getItem(USER_KEY)
      if (token) this.token = token
      if (rawUser) {
        try {
          this.user = JSON.parse(rawUser)
        } catch {
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
     * @returns {Promise<object>} 登录响应的 data
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

    async logout() {
      try {
        await authApi.logout()
      } catch {
        // 后端不可用 / 令牌已失效都不影响本地登出
      }
      this.clear()
    },

    clear() {
      this.token = ''
      this.user = null
      this.role = ''
      this.restored = true
      localStorage.removeItem(TOKEN_KEY)
      localStorage.removeItem(USER_KEY)
    },

    /** 主动刷新用户信息（一定会发请求；并发不合并，调用方自己决定要不要节流） */
    async fetchMe() {
      const me = await authApi.getMe()
      if (me) {
        this.user = { ...(this.user || {}), ...me }
        this.role = me.role || this.role
        localStorage.setItem(USER_KEY, JSON.stringify(this.user))
        this.lastFetchAt = Date.now()
      }
      return me
    },

    /**
     * 节流刷新：距上次成功刷新超过 ttlMs 才真正请求；并发调用复用同一个 Promise。
     *
     * 为什么需要它：`user` 是登录那一刻的快照，管理员事后调分/拉黑不会自己同步。
     * 但也不能见页面就发请求 —— 首页每次切回来都打一次 /auth/me 纯属浪费。
     * 折中：**短 TTL 节流**（默认 15s）+ **并发去重**（照抄 dict.ensureLoaded 的写法）。
     *
     * @param {number} ttlMs 0 表示必刷（「我的」页要立刻看到新数字，不该被节流挡住）
     * @returns {Promise<object|null>} 刷新后的 user；节流命中或失败时返回 null
     * @note **失败静默**：本地快照足以让页面正常显示，不能因为刷新失败就报错/白屏
     */
    async refreshIfStale(ttlMs = 15000) {
      const now = Date.now()
      // ① 节流：刚刷过就直接返回（ttl=0 时恒不命中，即必刷）
      if (this.lastFetchAt && now - this.lastFetchAt < ttlMs) {
        return null
      }
      // ② 并发去重：同时多处调用只发一次请求
      if (this._inflight) return this._inflight

      this._inflight = (async () => {
        try {
          return await this.fetchMe()
        } catch {
          return null // 静默失败：继续用本地快照
        } finally {
          this._inflight = null
        }
      })()
      return this._inflight
    },
  },
})
