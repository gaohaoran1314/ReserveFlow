// =============================================================================
// src/stores/dict.js  —  资源字典缓存（types / statuses / locations）
// 与 admin 版同构：用「共享 in-flight Promise」去重，并发调用只发 1 次 /resources/meta
// =============================================================================
import { defineStore } from 'pinia'

import { getMeta } from '../api/resource'

export const useDictStore = defineStore('dict', {
  state: () => ({
    types: [],
    statuses: [],
    locations: [],
    loading: false,
    loadedAt: 0,
  }),

  getters: {
    loaded: (s) => s.loadedAt > 0,
  },

  actions: {
    /**
     * 保证字典已加载（并发安全）
     * @param {boolean} [force]
     */
    async ensureLoaded(force = false) {
      if (!force && this.loadedAt > 0) return
      if (this._inflight) return this._inflight

      this.loading = true
      this._inflight = (async () => {
        try {
          const meta = await getMeta()
          this.types = meta?.types || []
          this.statuses = meta?.statuses || []
          this.locations = meta?.locations || []
          this.loadedAt = Date.now()
        } finally {
          this.loading = false
          this._inflight = null
        }
      })()
      return this._inflight
    },
  },
})
