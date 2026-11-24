<template>
  <div>
    <van-nav-bar title="我的候补" />

    <van-tabs v-model:active="activeStatus" sticky @change="onTabChange">
      <van-tab title="全部" name="" />
      <van-tab v-for="s in WAITLIST_STATUS" :key="s.value" :title="s.label" :name="s.value" />
    </van-tabs>

    <van-pull-refresh v-model="refreshing" @refresh="onRefresh">
      <van-list
        v-model:loading="loading"
        :finished="finished"
        finished-text="没有更多了"
        :immediate-check="false"
        @load="loadMore"
      >
        <div class="rf-page">
          <div v-for="item in sortedList" :key="item.id" class="rf-card">
            <!-- 头部：资源 + 状态 -->
            <div class="rf-card-head">
              <span class="rf-emoji">{{ emojiOf(item) }}</span>
              <div class="rf-grow" style="min-width: 0">
                <div class="rf-card-title">{{ resourceTitle(item) }}</div>
                <div class="rf-card-sub rf-mono">
                  {{ item.resourceCode || `#${item.resourceId}` }}
                </div>
              </div>
              <StatusTag type="waitlist" :value="item.status" :label="item.statusLabel" />
            </div>

            <!-- 期望时段 -->
            <div class="rf-card-sub">
              🕐 {{ item.slotDate }} {{ slotRangeLabel(item.startIndex, item.endIndex) }}
            </div>

            <!-- 排名（仅排队中） -->
            <div v-if="item.status === 'WAITING'" class="rf-card-sub">
              📋 你前面还有
              <strong style="color: var(--rf-primary)">
                {{ Math.max(0, (item.position || 1) - 1) }}
              </strong>
              位
            </div>

            <!-- offer 倒计时（重点：本地 1s 计时，不轮询） -->
            <div v-if="item.status === 'OFFERED'" class="rf-offer">
              <div class="rf-offer-hd">
                <van-icon name="fire" color="#ff976a" />
                <span>有名额了，请在倒计时内确认</span>
              </div>
              <div class="rf-countdown">{{ countdownOf(item) }}</div>
              <div class="rf-card-sub">逾期未确认，名额将转给下一位</div>
            </div>

            <!-- 已转正：跳到对应预约 -->
            <div v-if="item.status === 'CONVERTED' && item.convertedReservationId" class="rf-card-sub">
              ✅ 已转为预约单
              <van-button
                size="mini"
                type="primary"
                plain
                style="margin-left: 6px"
                @click="goReservation(item.convertedReservationId)"
              >
                查看预约
              </van-button>
            </div>

            <!-- 动作 -->
            <div v-if="item.status === 'OFFERED' || item.status === 'WAITING'" class="rf-card-foot">
              <span class="rf-muted" style="font-size: 12px">
                加入于 {{ formatTime(item.createdAt, false) }}
              </span>
              <div class="rf-row">
                <van-button
                  v-if="item.status === 'OFFERED'"
                  size="small"
                  type="primary"
                  :loading="acting === item.id"
                  @click="onConfirm(item)"
                >
                  确认名额
                </van-button>
                <van-button
                  size="small"
                  plain
                  type="danger"
                  :loading="acting === item.id"
                  @click="onLeave(item)"
                >
                  退出候补
                </van-button>
              </div>
            </div>
            <div v-else class="rf-card-foot">
              <span class="rf-muted" style="font-size: 12px">
                更新于 {{ formatTime(item.updatedAt, false) }}
              </span>
            </div>
          </div>

          <EmptyState
            v-if="!loading && !sortedList.length"
            icon="⏳"
            :text="emptyText"
            action-text="去首页看看资源"
            @action="router.replace({ name: 'home' })"
          />
        </div>
      </van-list>
    </van-pull-refresh>
  </div>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { showConfirmDialog, showSuccessToast, showToast } from 'vant'

import StatusTag from '../components/StatusTag.vue'
import EmptyState from '../components/EmptyState.vue'
import { confirmOffer, leaveWaitlist, listWaitlist } from '../api/waitlist'
import { RESOURCE_EMOJI } from '../constants/enums'
import { formatTime, slotRangeLabel } from '../utils/format'
import { countdownText, secondsLeft } from '../utils/datetime'

const router = useRouter()

/** 候补状态（后端 WaitlistStatus 5 值；后端已给 statusLabel，这里只用于 Tab） */
const WAITLIST_STATUS = [
  { value: 'WAITING', label: '排队中' },
  { value: 'OFFERED', label: '已获名额' },
  { value: 'CONVERTED', label: '已转正' },
  { value: 'CANCELLED', label: '已退出' },
  { value: 'EXPIRED', label: '已过期' },
]

// ---------------------------------------------------------------- 列表
const activeStatus = ref('')
const list = ref([])
const page = ref(1)
const size = 10
const total = ref(0)
const loading = ref(false)
const finished = ref(false)
const refreshing = ref(false)
const acting = ref(0)

const emptyText = computed(() =>
  activeStatus.value ? '该状态下没有候补记录' : '还没有候补记录'
)

/**
 * 排序：OFFERED 置顶（有时效性）→ WAITING → 其余按创建时间倒序。
 * 「全部」Tab 下这样读起来最省心；后端不排序，前端做这一层展示排序。
 */
const ORDER = { OFFERED: 0, WAITING: 1, CONVERTED: 2, CANCELLED: 3, EXPIRED: 4 }
const sortedList = computed(() => {
  if (activeStatus.value) return list.value
  return [...list.value].sort((a, b) => {
    const d = (ORDER[a.status] ?? 9) - (ORDER[b.status] ?? 9)
    if (d !== 0) return d
    return String(b.createdAt).localeCompare(String(a.createdAt))
  })
})

async function fetchPage() {
  const params = { page: page.value, size }
  if (activeStatus.value) params.status = activeStatus.value
  const data = await listWaitlist(params)
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

function onTabChange() {
  reload()
}

function onRefresh() {
  refreshing.value = true
  reload()
  refreshing.value = false
}

// ---------------------------------------------------------------- 倒计时
/**
 * 本地 1 秒计时（**不轮询接口**）。
 * 注意两件事：
 *   1) onBeforeUnmount 必须 clearInterval，否则在 TabBar 来回切会叠计时器；
 *   2) 后台标签页里 setInterval 会被浏览器节流到 1 分钟/次，
 *      所以在 visibilitychange 时立刻把 now 拉回真实时间（否则切回来显示的是旧值）。
 */
const now = ref(Date.now())
let timer = null
const expiredHandled = reactive(new Set())

function startTimer() {
  if (timer) clearInterval(timer)
  timer = setInterval(() => {
    now.value = Date.now()
  }, 1000)
}

function onVisibilityChange() {
  if (document.visibilityState === 'visible') {
    now.value = Date.now()
    // 切回前台时顺手刷新一次，避免错过到期（成本很低，一次 GET）
    reload()
  }
}

function countdownOf(item) {
  if (!item.offerExpiresAt) return '—'
  return countdownText(item.offerExpiresAt, now.value)
}

/** 每秒检查：有 offer 到期就刷新一次并提示（每个 id 只处理一次） */
function checkExpiry() {
  for (const item of list.value) {
    if (item.status !== 'OFFERED' || !item.offerExpiresAt) continue
    if (expiredHandled.has(item.id)) continue
    if (secondsLeft(item.offerExpiresAt, now.value) <= 0) {
      expiredHandled.add(item.id)
      showToast('名额已过期')
      reload()
      return
    }
  }
}

// ---------------------------------------------------------------- 动作
async function onConfirm(item) {
  acting.value = item.id
  try {
    const res = await confirmOffer(item.id)
    showSuccessToast('已确认，名额已锁定')
    const rid = res?.convertedReservationId
    await reload()
    if (rid) router.push({ name: 'reservations', query: { id: String(rid) } })
  } catch {
    // 3021 offer 过期 / 3019 状态不允许：拦截器已提示，刷新让状态跟上
    await reload()
  } finally {
    acting.value = 0
  }
}

async function onLeave(item) {
  try {
    await showConfirmDialog({
      title: '退出候补',
      message: `确认退出「${resourceTitle(item)}」${item.slotDate} 的候补队列？退出后需重新排队。`,
      confirmButtonText: '退出',
      confirmButtonColor: '#ee0a24',
    })
  } catch {
    return
  }

  acting.value = item.id
  try {
    await leaveWaitlist(item.id)
    showSuccessToast('已退出候补')
    await reload()
  } catch {
    await reload()
  } finally {
    acting.value = 0
  }
}

function goReservation(id) {
  router.push({ name: 'reservations', query: { id: String(id) } })
}

// ---------------------------------------------------------------- 展示辅助
/** 资源名可能为空：resourceName → resourceCode → 资源 #id */
function resourceTitle(item) {
  return item.resourceName || item.resourceCode || `资源 #${item.resourceId}`
}

function emojiOf(item) {
  return RESOURCE_EMOJI[item.resourceType] || '📦'
}

// ---------------------------------------------------------------- 生命周期
let watchTimer = null

onMounted(async () => {
  await reload()
  startTimer()
  document.addEventListener('visibilitychange', onVisibilityChange)
  // 复用同一个 1s 节拍做到期检查（不额外开定时器）
  watchTimer = setInterval(checkExpiry, 1000)
})

onBeforeUnmount(() => {
  if (timer) clearInterval(timer)
  if (watchTimer) clearInterval(watchTimer)
  timer = null
  watchTimer = null
  document.removeEventListener('visibilitychange', onVisibilityChange)
})

</script>

<style scoped>
.rf-offer {
  margin-top: 8px;
  padding: 10px 12px;
  background: #fff7e8;
  border: 1px solid #ffe0b2;
  border-radius: 8px;
  text-align: center;
}

.rf-offer-hd {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 4px;
  font-size: 12px;
  color: #d46b08;
}

.rf-countdown {
  font-size: 28px;
  font-weight: 700;
  line-height: 1.3;
  color: #fa541c;
  font-variant-numeric: tabular-nums;
  letter-spacing: 1px;
}
</style>
