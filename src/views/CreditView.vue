<template>
  <div>
    <van-nav-bar title="我的信用" left-arrow @click-left="goBack" />

    <div class="rf-page">
      <!-- 信用分环 -->
      <div class="rf-card rf-credit-card">
        <van-circle
          v-model:current-rate="rate"
          :rate="score"
          :speed="60"
          :stroke-width="80"
          layer-color="#ebedf0"
          :color="ringColor"
          size="132px"
          :text="`${score}`"
        />
        <div class="rf-credit-meta">
          <StatusTag type="user" :value="status" :label="statusLabel" />
          <div class="rf-credit-hint">{{ hint }}</div>
        </div>
      </div>

      <!-- 阈值说明 -->
      <van-cell-group inset>
        <van-cell title="进黑名单" :value="`低于 ${enterScore} 分`" />
        <van-cell title="解除黑名单" :value="`达到 ${exitScore} 分`" />
        <van-cell
          title="距离黑名单"
          :value="distanceToBlacklist > 0 ? `再扣 ${distanceToBlacklist} 分` : '—'"
        />
        <van-cell
          title="距离恢复"
          :value="distanceToRecover > 0 ? `还需 ${distanceToRecover} 分` : '—'"
        />
      </van-cell-group>

      <div class="rf-muted" style="padding: 8px 4px; font-size: 12px; line-height: 1.7">
        {{ 60 <= score && score < 80 ? '当前处于迟滞区：分数在 60–79 之间时保持原状态，防止临界反复横跳。' : '' }}
      </div>

      <!-- 变动明细 -->
      <div class="rf-row-between" style="margin: 12px 4px 8px">
        <span style="font-weight: 600">信用变动明细</span>
        <van-dropdown-menu style="min-width: 130px">
          <van-dropdown-item v-model="reasonType" :options="reasonOptions" @change="reload" />
        </van-dropdown-menu>
      </div>

      <van-list
        v-model:loading="loading"
        :finished="finished"
        finished-text="没有更多了"
        :immediate-check="false"
        @load="loadMore"
      >
        <div v-for="log in list" :key="log.id" class="rf-card">
          <div class="rf-row-between">
            <span style="font-weight: 500">
              <!-- 优先用后端 reasonLabel（已中文化）；reasonType 只用于筛选 -->
              {{ log.reasonLabel || log.reasonType }}
            </span>
            <span class="rf-delta" :class="{ 'is-minus': log.changeScore < 0 }">
              {{ signText(log.changeScore) }}
            </span>
          </div>
          <div class="rf-card-sub">
            {{ log.scoreBefore }} → {{ log.scoreAfter }} 分
            <span v-if="log.statusBefore !== log.statusAfter" style="margin-left: 6px; color: #ee0a24">
              （{{ statusText(log.statusBefore) }} → {{ statusText(log.statusAfter) }}）
            </span>
          </div>
          <div v-if="log.ruleCode" class="rf-card-sub rf-mono" style="font-size: 11px">
            规则 {{ log.ruleCode }}
          </div>
          <div v-if="log.remark" class="rf-card-sub">{{ log.remark }}</div>
          <div class="rf-card-sub" style="font-size: 11px">
            {{ formatTime(log.createdAt) }}
            <van-button
              v-if="log.reservationId"
              size="mini"
              type="primary"
              plain
              style="margin-left: 6px"
              @click="goReservation(log.reservationId)"
            >
              查看预约
            </van-button>
          </div>
        </div>

        <EmptyState
          v-if="!loading && !list.length"
          icon="⭐"
          :text="reasonType ? '该原因下没有记录' : '还没有信用变动记录'"
          :action-text="reasonType ? '查看全部' : ''"
          @action="reasonType = ''; reload()"
        />
      </van-list>
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { showFailToast } from 'vant'

import StatusTag from '../components/StatusTag.vue'
import EmptyState from '../components/EmptyState.vue'
import { getMyCredit, listMyCreditLogs } from '../api/credit'
import { CREDIT_REASON, USER_STATUS, labelOf } from '../constants/enums'
import { formatTime, signText } from '../utils/format'

const router = useRouter()

// ---------------------------------------------------------------- 概览
const score = ref(0)
const rate = ref(0)
const status = ref('')
const statusLabel = ref('')
const hint = ref('')
const enterScore = ref(60)
const exitScore = ref(80)
const distanceToBlacklist = ref(0)
const distanceToRecover = ref(0)

const ringColor = computed(() => {
  const s = score.value
  if (s < 60) return '#ee0a24'
  if (s < 80) return '#ff976a'
  return '#07c160'
})

async function loadCredit() {
  try {
    const d = await getMyCredit()
    score.value = d?.score ?? 0
    status.value = d?.status || ''
    statusLabel.value = d?.statusLabel || labelOf(USER_STATUS, status.value)
    hint.value = d?.hint || ''
    enterScore.value = d?.enterScore ?? 60
    exitScore.value = d?.exitScore ?? 80
    distanceToBlacklist.value = d?.distanceToBlacklist ?? 0
    distanceToRecover.value = d?.distanceToRecover ?? 0
  } catch {
    showFailToast('信用信息加载失败')
  }
}

// ---------------------------------------------------------------- 明细
const reasonType = ref('')
const list = ref([])
const page = ref(1)
const size = 20
const total = ref(0)
const loading = ref(false)
const finished = ref(false)

const reasonOptions = [
  { text: '全部原因', value: '' },
  ...CREDIT_REASON.map((r) => ({ text: r.label, value: r.value })),
]

function statusText(v) {
  return labelOf(USER_STATUS, v)
}

async function fetchPage() {
  const params = { page: page.value, size }
  if (reasonType.value) params.reasonType = reasonType.value
  const data = await listMyCreditLogs(params)
  const rows = data?.list || []
  total.value = data?.total || 0
  list.value = page.value === 1 ? rows : list.value.concat(rows)
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
  return loadMore()
}

function goReservation(id) {
  router.push({ name: 'reservations', query: { id: String(id) } })
}

function goBack() {
  if (window.history.length > 1) router.back()
  else router.replace({ name: 'profile' })
}

onMounted(async () => {
  await loadCredit()
  await loadMore()
})
</script>

<style scoped>
.rf-credit-card {
  display: flex;
  align-items: center;
  gap: 16px;
}

.rf-credit-meta {
  flex: 1;
  min-width: 0;
}

.rf-credit-hint {
  margin-top: 6px;
  font-size: 13px;
  color: var(--rf-text-2);
  line-height: 1.6;
}

.rf-delta {
  font-size: 17px;
  font-weight: 700;
  color: #07c160;
  font-variant-numeric: tabular-nums;
}

.rf-delta.is-minus {
  color: #ee0a24;
}
</style>
