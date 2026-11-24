<template>
  <div>
    <PageHeader title="概览" desc="系统关键指标一览。指标来自实时接口，进入页面即拉取。">
      <template #actions>
        <el-button :loading="loading" @click="loadAll">刷新</el-button>
      </template>
    </PageHeader>

    <!-- 当前登录账号 -->
    <el-card shadow="never" style="margin-bottom: 12px">
      <div style="display: flex; align-items: center; gap: 16px; flex-wrap: wrap">
        <el-avatar :size="44">{{ (auth.displayName || 'A').slice(0, 1).toUpperCase() }}</el-avatar>
        <div>
          <div style="font-size: 16px; font-weight: 600">
            {{ auth.displayName || '—' }}
            <el-tag size="small" type="danger" style="margin-left: 6px">
              {{ auth.isAdmin ? '管理员' : '普通用户' }}
            </el-tag>
          </div>
          <div class="rf-muted" style="font-size: 13px; margin-top: 2px">
            信用分 {{ auth.creditScore ?? '—' }} · 账号 ID {{ auth.user?.id ?? '—' }}
          </div>
        </div>
      </div>
    </el-card>

    <!-- 5 张统计卡（全部走现有接口，零后端改动） -->
    <div class="rf-stat-row">
      <el-card v-for="c in cards" :key="c.key" shadow="never" class="rf-stat-card" v-loading="loading">
        <div class="rf-stat-value" :style="{ color: c.color }">{{ c.value }}</div>
        <div class="rf-stat-label">{{ c.label }}</div>
      </el-card>
    </div>

    <el-card shadow="never" style="margin-top: 12px">
      <template #header>
        <div style="display: flex; justify-content: space-between; align-items: center">
          <span>后续功能</span>
          <el-tag size="small" type="info">T12 实现</el-tag>
        </div>
      </template>
      <EmptyState
        title="7 天新增预约趋势"
        desc="按 createdAt 分桶的纯 CSS 柱状图将在 T12（概览页收口）实现；当前批次只交付脚手架、路由、HTTP 客户端、登录闭环与通用组件。"
        icon="TrendCharts"
      />
    </el-card>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'

import PageHeader from '../components/PageHeader.vue'
import EmptyState from '../components/EmptyState.vue'
import { useAuthStore } from '../stores/auth'
import { listResources } from '../api/resource'
import { listReservations } from '../api/reservation'
import { listUsers } from '../api/system'

const auth = useAuthStore()
const loading = ref(false)

const stats = reactive({
  resources: 0,
  active: 0,
  pendingReview: 0,
  dueReturn: 0,
  blacklist: 0,
})

const cards = computed(() => [
  { key: 'resources', label: '资源总数', value: stats.resources, color: '#409eff' },
  { key: 'active', label: '有效预约', value: stats.active, color: '#67c23a' },
  { key: 'pending', label: '待审核', value: stats.pendingReview, color: '#e6a23c' },
  { key: 'due', label: '逾期未还', value: stats.dueReturn, color: '#f56c6c' },
  { key: 'blacklist', label: '黑名单人数', value: stats.blacklist, color: '#909399' },
])

const ACTIVE_STATUSES = ['PENDING_REVIEW', 'LOCKED', 'IN_USE', 'DUE_RETURN', 'RETURN_CHECK']

async function loadAll() {
  loading.value = true
  try {
    // 后端不支持多状态一次查，5 个状态并发（单实例下总计 <100ms）
    const activeReqs = ACTIVE_STATUSES.map((s) => listReservations({ status: s, page: 1, size: 1 }))

    const [resources, ...rest] = await Promise.all([
      listResources({ page: 1, size: 1 }),
      ...activeReqs,
      listReservations({ status: 'PENDING_REVIEW', page: 1, size: 1 }),
      listReservations({ status: 'DUE_RETURN', page: 1, size: 1 }),
      listUsers({ status: 'BLACKLIST', page: 1, size: 1 }),
    ])

    const activePages = rest.slice(0, ACTIVE_STATUSES.length)
    const pendingPage = rest[ACTIVE_STATUSES.length]
    const duePage = rest[ACTIVE_STATUSES.length + 1]
    const blackPage = rest[ACTIVE_STATUSES.length + 2]

    stats.resources = resources?.total ?? 0
    stats.active = activePages.reduce((sum, p) => sum + (p?.total ?? 0), 0)
    stats.pendingReview = pendingPage?.total ?? 0
    stats.dueReturn = duePage?.total ?? 0
    stats.blacklist = blackPage?.total ?? 0
  } catch {
    // 具体错误提示已由 http 拦截器统一给出
  } finally {
    loading.value = false
  }
}

onMounted(loadAll)
</script>

<style scoped>
/* 5 等分用 flex 而不是 el-col 的 span（24 不能被 5 整除） */
.rf-stat-row {
  display: flex;
  gap: 12px;
}

.rf-stat-row .rf-stat-card {
  flex: 1 1 0;
  min-width: 0;
}
</style>
