<template>
  <div>
    <van-nav-bar title="我的预约" />

    <van-tabs v-model:active="activeStatus" sticky @change="onTabChange">
      <van-tab title="全部" name="" />
      <van-tab v-for="s in filterStatuses" :key="s.value" :title="s.label" :name="s.value" />
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
          <ReservationCard
            v-for="item in list"
            :key="item.id"
            :item="item"
            @detail="openDetail"
            @action="onQuickAction"
          />

          <EmptyState
            v-if="!loading && !list.length"
            icon="🗓"
            :text="activeStatus ? '该状态下没有预约' : '还没有预约记录'"
            action-text="去首页看看"
            @action="router.replace({ name: 'home' })"
          />
        </van-list>
      </van-pull-refresh>
    </div>

    <!-- ==================== 详情弹层 ==================== -->
    <van-popup v-model:show="popup.visible" position="bottom" round :style="{ height: '88%' }">
      <div class="rf-popup">
        <div class="rf-popup-hd">
          <span class="rf-popup-title">{{ detail?.reservationNo || '预约详情' }}</span>
          <StatusTag
            v-if="detail"
            type="reservation"
            :value="detail.status"
            :label="detail.statusLabel"
          />
        </div>

        <div class="rf-popup-body">
          <template v-if="detail">
            <!-- 基本信息 -->
            <van-cell-group inset>
              <van-cell title="资源" :value="detail.resourceName" />
              <van-cell title="资源编号" :value="detail.resourceCode" class="rf-mono" />
              <van-cell title="使用时段" :value="formatRange(detail.startTime, detail.endTime)" />
              <van-cell title="押金" :value="depositText" />
              <van-cell v-if="detail.remark" title="备注" :label="detail.remark" />
              <van-cell v-if="detail.needReview" title="审核" value="需管理员审核" />
            </van-cell-group>

            <!-- 押金流水 -->
            <div class="rf-section-title">押金流水</div>
            <div v-if="!deposits.list.length" class="rf-muted" style="padding: 4px 4px 8px">
              该资源无押金，没有产生任何押金流水。
            </div>
            <van-cell-group v-else inset>
              <van-cell
                v-for="d in deposits.list"
                :key="d.id"
                :title="labelOf(DEPOSIT_RECORD_TYPE, d.recordType)"
                :label="`${d.transactionNo} · ${formatTime(d.createdAt)}`"
              >
                <template #value>
                  <span :style="{ color: recordColor(d.recordType) }">
                    ¥{{ formatMoney(d.amount) }}
                  </span>
                </template>
              </van-cell>
            </van-cell-group>

            <!-- 状态时间线 -->
            <div class="rf-section-title">状态时间线</div>
            <div v-if="!timeline.length" class="rf-muted" style="padding: 4px 4px 8px">
              暂无状态变更记录
            </div>
            <div v-else class="rf-timeline">
              <div v-for="(n, i) in timeline" :key="i" class="rf-timeline-item">
                <div class="rf-timeline-dot" :class="{ 'is-first': i === 0 }"></div>
                <div class="rf-timeline-main">
                  <div class="rf-row" style="gap: 4px; flex-wrap: wrap">
                    <StatusTag
                      v-if="n.fromStatus"
                      type="reservation"
                      :value="n.fromStatus"
                      plain
                    />
                    <span v-else class="rf-muted" style="font-size: 12px">创建</span>
                    <span class="rf-muted">→</span>
                    <StatusTag type="reservation" :value="n.toStatus" />
                    <van-tag v-if="n.ruleCode" type="warning" plain>{{ n.ruleCode }}</van-tag>
                  </div>
                  <div v-if="n.reason" class="rf-card-sub">{{ n.reason }}</div>
                  <div class="rf-card-sub" style="font-size: 11px">
                    {{ formatTime(n.createdAt) }} · {{ n.operatorId ? `操作人 #${n.operatorId}` : '系统' }}
                  </div>
                </div>
              </div>
            </div>
          </template>

          <van-loading v-else size="20" style="padding: 24px 0; text-align: center">
            加载中…
          </van-loading>
        </div>

        <!-- 底部动作（以详情的 allowedActions 为准；缺失时按 status 兜底） -->
        <div class="rf-popup-ft">
          <van-button block plain @click="popup.visible = false">关闭</van-button>
          <van-button
            v-for="b in buttons"
            :key="b.code"
            block
            :type="b.type"
            :loading="acting === b.code"
            @click="onAction(b)"
          >
            {{ b.label }}
          </van-button>
        </div>
      </div>
    </van-popup>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { showConfirmDialog, showFailToast, showSuccessToast } from 'vant'

import ReservationCard from '../components/ReservationCard.vue'
import StatusTag from '../components/StatusTag.vue'
import EmptyState from '../components/EmptyState.vue'

import * as reservationApi from '../api/reservation'
import {
  DEPOSIT_RECORD_TYPE,
  RESERVATION_STATUS,
  RESERVATION_STATUS_ACTIONS,
  STUDENT_ACTIONS,
  labelOf,
} from '../constants/enums'
import { formatMoney, formatRange, formatTime } from '../utils/format'

const route = useRoute()
const router = useRouter()

// ---------------------------------------------------------------- 列表
const activeStatus = ref('')
const list = ref([])
const page = ref(1)
const size = 10
const total = ref(0)
const loading = ref(false)
const finished = ref(false)
const refreshing = ref(false)

/** 状态筛选 Tab：全部 + 待审核/已锁定/使用中/待归还/已完成（按 lead 的 6 个 Tab） */
const FILTER_VALUES = ['PENDING_REVIEW', 'LOCKED', 'IN_USE', 'DUE_RETURN', 'COMPLETED']
const filterStatuses = RESERVATION_STATUS.filter((s) => FILTER_VALUES.includes(s.value))

async function fetchPage() {
  const params = { page: page.value, size }
  if (activeStatus.value) params.status = activeStatus.value

  const data = await reservationApi.listReservations(params)
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

// ---------------------------------------------------------------- 详情弹层
const popup = reactive({ visible: false })
const detail = ref(null)
const timeline = ref([])
const deposits = reactive({ account: null, list: [] })
const acting = ref('')

const depositText = computed(() => {
  const d = Number(detail.value?.depositAmount || 0)
  return d > 0 ? `¥${d.toFixed(2)}` : '免费'
})

/**
 * 动作按钮：
 *   详情接口给了 allowedActions 就以它为准（服务端裁决）；
 *   万一字段缺失，才用 RESERVATION_STATUS_ACTIONS 按 status 兜底。
 */
const buttons = computed(() => {
  const d = detail.value
  if (!d) return []
  const raw = Array.isArray(d.allowedActions) ? d.allowedActions : null
  const codes = raw
    ? raw.filter((c) => STUDENT_ACTIONS.includes(c))
    : [RESERVATION_STATUS_ACTIONS[d.status]?.code].filter(Boolean)

  return codes.map((code) => ({
    code,
    label: ACTION_LABEL[code] || code,
    type: code === 'CANCEL' ? 'danger' : 'primary',
  }))
})

const ACTION_LABEL = {
  CANCEL: '取消预约',
  START_USE: '立即取用',
  SUBMIT_RETURN: '归还',
}

async function openDetail(item) {
  const id = typeof item === 'number' ? item : item?.id
  if (!id) return
  popup.visible = true
  detail.value = null
  timeline.value = []
  deposits.account = null
  deposits.list = []
  await loadDetail(id)
}

async function loadDetail(id) {
  try {
    const [d, dep, tl] = await Promise.all([
      reservationApi.getReservation(id),
      reservationApi.listDeposits(id).catch(() => null),
      // timeline 接口若不可用（404）不影响详情
      fetchTimeline(id),
    ])
    detail.value = d || null
    if (dep) {
      deposits.account = dep.account || null
      deposits.list = dep.list || []
    }
    timeline.value = tl
  } catch {
    popup.visible = false
  }
}

/** 时间线（T00 新增接口；失败时返回空数组，页面显示「暂无记录」） */
async function fetchTimeline(id) {
  try {
    const data = await reservationApi.getTimeline(id)
    return data?.list || []
  } catch {
    return []
  }
}

// ---------------------------------------------------------------- 三个动作
async function onAction(btn) {
  const d = detail.value
  if (!d) return

  const label = ACTION_LABEL[btn.code] || btn.code
  try {
    await showConfirmDialog({
      title: label,
      message: `确认对预约 ${d.reservationNo} 执行「${label}」？`,
      confirmButtonText: label,
      cancelButtonText: '取消',
    })
  } catch {
    return
  }

  acting.value = btn.code
  try {
    if (btn.code === 'CANCEL') await reservationApi.cancelReservation(d.id, { reason: '用户主动取消' })
    else if (btn.code === 'START_USE') await reservationApi.pickupReservation(d.id)
    else if (btn.code === 'SUBMIT_RETURN') await reservationApi.returnReservation(d.id)

    showSuccessToast(`${label}成功`)
    // 详情与列表都刷新（状态、按钮、押金、时间线一起变）
    await loadDetail(d.id)
    await reload()
  } catch (e) {
    // 3007 状态已变 / 3015 未到取用时间 / 3016 已过取用时限 → 以服务端为准重拉详情
    if ([3007, 3015, 3016, 3008].includes(e?.code)) {
      await loadDetail(d.id)
      await reload()
    }
  } finally {
    acting.value = ''
  }
}

/** 列表卡片上的快捷按钮（按 status 推导的那一个动作） */
async function onQuickAction(code, item) {
  if (!item) return
  // 走统一的详情 + 动作流程，保证以服务端 allowedActions 为准
  await openDetail(item)
  const btn = buttons.value.find((b) => b.code === code)
  if (btn) await onAction(btn)
}

// ---------------------------------------------------------------- 生命周期
onMounted(async () => {
  await reload()
  // 支持 /reservations?id=123 直开详情（登录后回跳 / 分享）
  const qid = Number(route.query.id)
  if (Number.isFinite(qid) && qid > 0) await openDetail(qid)
})

function recordColor(t) {
  if (t === 'DEDUCT') return '#ee0a24'
  if (t === 'REFUND') return '#07c160'
  return '#ff976a'
}
</script>

<style scoped>
.rf-popup {
  display: flex;
  flex-direction: column;
  height: 100%;
}

.rf-popup-hd {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 14px 16px 10px;
  border-bottom: 1px solid #f2f3f5;
}

.rf-popup-title {
  font-size: 16px;
  font-weight: 600;
  flex: 1;
}

.rf-popup-body {
  flex: 1;
  overflow-y: auto;
  padding: 8px 0 12px;
}

.rf-popup-ft {
  display: flex;
  gap: 8px;
  padding: 10px 12px calc(10px + var(--safe-bottom));
  border-top: 1px solid #f2f3f5;
  background: #fff;
}

.rf-timeline {
  padding: 0 4px;
}

.rf-timeline-item {
  display: flex;
  gap: 8px;
  padding: 6px 0;
}

.rf-timeline-dot {
  flex: 0 0 8px;
  width: 8px;
  height: 8px;
  margin-top: 6px;
  border-radius: 50%;
  background: #dcdee0;
}

.rf-timeline-dot.is-first {
  background: var(--rf-primary);
}

.rf-timeline-main {
  flex: 1;
  min-width: 0;
  border-bottom: 1px dashed #f2f3f5;
  padding-bottom: 6px;
}
</style>
