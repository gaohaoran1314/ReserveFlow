// =============================================================================
// src/stores/dict.js  —  资源字典缓存（meta：types / statuses / locations）
// 对应 ARCH-P4 §4
//
// 为什么必须有：资源列表的筛选（类型/状态/地点）与新增表单都依赖 meta；
// 5 个页面各拉一次会重复请求，且 locations 会随资源增删变化。
// `ensureLoaded()` 用「共享同一个 in-flight Promise」做去重，并发调用只发 1 次请求。
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
    error: '',
  }),

  getters: {
    /** 「状态标记」下拉只允许后端标了 selectable 的项（实测仅 FREE / MAINTENANCE） */
    selectableStatuses: (s) => s.statuses.filter((x) => x.selectable),
    loaded: (s) => s.loadedAt > 0,
  },

  actions: {
    /**
     * 保证字典已加载（并发安全，只成功拉一次）
     * @param {boolean} [force] true 时强制刷新（资源增删后调）
     */
    async ensureLoaded(force = false) {
      if (!force && this.loadedAt > 0) return
      // 复用进行中的请求：并发调用只发一次
      if (this._inflight) return this._inflight

      this.loading = true
      this.error = ''
      this._inflight = (async () => {
        try {
          const meta = await getMeta()
          this.types = meta?.types || []
          this.statuses = meta?.statuses || []
          this.locations = meta?.locations || []
          this.loadedAt = Date.now()
        } catch (e) {
          this.error = e?.msg || '字典加载失败'
          throw e
        } finally {
          this.loading = false
          this._inflight = null
        }
      })()
      return this._inflight
    },

    /** 资源增删改后调用，让地点候选与状态选项跟上 */
    async refresh() {
      return this.ensureLoaded(true)
    },
  },
})
