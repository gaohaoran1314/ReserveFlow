// =============================================================================
// src/stores/ui.js  —  全局 UI 状态（顶层 loading 计数、侧边栏折叠）
// 折叠状态落 localStorage，刷新后保持用户的偏好。
// =============================================================================
import { defineStore } from 'pinia'

import { SIDEBAR_KEY } from '../constants/storage'

export const useUiStore = defineStore('ui', {
  state: () => ({
    loadingCount: 0,
    sidebarCollapsed: localStorage.getItem(SIDEBAR_KEY) === '1',
  }),

  getters: {
    loading: (s) => s.loadingCount > 0,
  },

  actions: {
    beginLoading() {
      this.loadingCount += 1
    },
    endLoading() {
      this.loadingCount = Math.max(0, this.loadingCount - 1)
    },
    /** 包一层异步调用，自动配对 begin/end（异常也会配对） */
    async wrap(fn) {
      this.beginLoading()
      try {
        return await fn()
      } finally {
        this.endLoading()
      }
    },
    toggleSidebar() {
      this.sidebarCollapsed = !this.sidebarCollapsed
      localStorage.setItem(SIDEBAR_KEY, this.sidebarCollapsed ? '1' : '0')
    },
  },
})
