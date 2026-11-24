<template>
  <div>
    <!-- 黑名单提示（主理人决策 3：notice-bar，不弹 Dialog） -->
    <van-notice-bar
      v-if="auth.isBlacklisted"
      left-icon="info-o"
      wrapable
      :scrollable="false"
      color="#ee0a24"
      background="#fff1f0"
      text="你的账号处于黑名单，暂时无法预约（可在「我的」查看信用）"
    />

    <van-search
      v-model="keyword"
      placeholder="搜索资源名称或编号"
      shape="round"
      @update:model-value="onKeywordChange"
      @search="reload"
      @clear="reload"
    />

    <!-- 分类 Tab：来自 /resources/meta 的 types（加一个「全部」） -->
    <van-tabs v-model:active="activeType" sticky @change="onTabChange">
      <van-tab title="全部" name="" />
      <van-tab v-for="t in dict.types" :key="t.value" :title="t.label" :name="t.value" />
    </van-tabs>

    <div class="rf-page">
      <van-pull-refresh v-model="refreshing" @refresh="onRefresh">
        <van-list
          v-model:loading="loading"
          :finished="finished"
          finished-text="没有更多了"
          :immediate-check="false"
          @load="loadMore"
        >
          <ResourceCard
            v-for="item in list"
            :key="item.id"
            :item="item"
            @click="goDetail"
          />

          <EmptyState
            v-if="!loading && !list.length"
            icon="🔍"
            :text="keyword ? '没有匹配的资源' : '暂无资源'"
            :action-text="keyword ? '清空搜索' : ''"
            @action="clearKeyword"
          />
        </van-list>
      </van-pull-refresh>
    </div>
  </div>
</template>

<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'

import ResourceCard from '../components/ResourceCard.vue'
import EmptyState from '../components/EmptyState.vue'
import { useAuthStore } from '../stores/auth'
import { useDictStore } from '../stores/dict'
import { listResources } from '../api/resource'

const router = useRouter()
const auth = useAuthStore()
const dict = useDictStore()

const list = ref([])
const page = ref(1)
const size = 10
const total = ref(0)
const loading = ref(false)
const finished = ref(false)
const refreshing = ref(false)

const keyword = ref('')
const activeType = ref('')

let searchTimer = null

/** 拉一页（page 由调用方维护） */
async function fetchPage() {
  const params = { page: page.value, size }
  if (activeType.value) params.type = activeType.value
  if (keyword.value.trim()) params.keyword = keyword.value.trim()

  const data = await listResources(params)
  const rows = data?.list || []
  total.value = data?.total || 0

  if (page.value === 1) {
    list.value = rows
  } else {
    list.value = list.value.concat(rows)
  }
  finished.value = list.value.length >= total.value || rows.length === 0
}

async function loadMore() {
  loading.value = true
  try {
    await fetchPage()
    if (!finished.value) page.value += 1
  } catch {
    finished.value = true
  } finally {
    loading.value = false
  }
}

function reload() {
  page.value = 1
  total.value = 0
  finished.value = false
  list.value = []
  loadMore()
}

function onRefresh() {
  refreshing.value = true
  page.value = 1
  finished.value = false
  list.value = []
  loadMore()
  refreshing.value = false
}

function onTabChange() {
  reload()
}

/** 搜索防抖 300ms（ARCH-P5 §5.2） */
function onKeywordChange() {
  if (searchTimer) clearTimeout(searchTimer)
  searchTimer = setTimeout(reload, 300)
}

function clearKeyword() {
  keyword.value = ''
  reload()
}

function goDetail(item) {
  router.push({ name: 'resource', params: { id: String(item.id) } })
}

/**
 * 从后台切回前台时刷新用户状态（信用分 / 黑名单）。
 *
 * 为什么需要：黑名单提示条读的是 store 里的登录快照，
 * 学生停在首页不动时，管理员把他拉黑了他也不会知道。
 * 这里用「切回标签页」这个时机补上 —— **不做定时轮询**：
 * 每 30 秒打一次 /auth/me 是纯浪费，而「切标签页/切路由」才是真实场景的常态。
 */
function onVisibilityChange() {
  if (document.visibilityState === 'visible') {
    auth.refreshIfStale() // 15s 节流 + 并发去重 + 失败静默，都在 store 里
  }
}

onMounted(async () => {
  // 字典与首屏列表并发拉取（dict 内部保证只发一次）
  dict.ensureLoaded().catch(() => {})
  // 进首页刷新一次用户状态（被 App.vue 冷启动刷过的话会被 15s 节流挡住，不产生多余请求）
  auth.refreshIfStale()
  document.addEventListener('visibilitychange', onVisibilityChange)
  await loadMore()
})

// 必须移除监听：否则来回切 Tab 会叠加 handler（与 WaitlistView 的倒计时同理）
onBeforeUnmount(() => {
  document.removeEventListener('visibilitychange', onVisibilityChange)
})
</script>
