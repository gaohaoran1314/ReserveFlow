<template>
  <div>
    <van-nav-bar :title="resource?.name || '资源详情'" left-arrow @click-left="goBack" />

    <div class="rf-page">
      <!-- 资源信息卡 -->
      <div class="rf-card">
        <div class="rf-card-head">
          <span class="rf-emoji">{{ emoji }}</span>
          <div class="rf-grow" style="min-width: 0">
            <div class="rf-card-title">{{ resource?.name || '加载中…' }}</div>
            <div class="rf-card-sub rf-mono">{{ resource?.code }}</div>
          </div>
          <StatusTag
            v-if="resource"
            type="resource"
            :value="resource.status"
            :label="resource.statusLabel"
          />
        </div>

        <div v-if="resource" class="rf-card-sub">
          <div>📍 {{ resource.location || '未填写地点' }}</div>
          <div>
            👥 可容纳 {{ resource.capacity }}{{ capacityUnit }} · 💰
            {{ resource.depositAmount > 0 ? `押金 ¥${formatMoney(resource.depositAmount)}` : '免费' }}
          </div>
          <div class="rf-mono" style="margin-top: 2px">{{ typeLabel }}</div>
        </div>
        <div v-if="resource?.description" class="rf-card-sub" style="margin-top: 6px">
          {{ resource.description }}
        </div>
      </div>

      <!-- 不可约提示 -->
      <van-notice-bar
        v-if="resource && !bookable"
        wrapable
        :scrollable="false"
        left-icon="warning-o"
        text="该资源当前不可预约（维护中 / 已停用）"
      />

      <!-- 日期切换：今天 + 未来 6 天 -->
      <div class="rf-date-bar">
        <div
          v-for="d in days"
          :key="d.date"
          class="rf-date-item"
          :class="{ 'is-active': d.date === date }"
          @click="pickDate(d.date)"
        >
          <div class="rf-date-label">{{ d.label }}</div>
          <div class="rf-date-week">{{ d.weekday }}</div>
        </div>
      </div>

      <!-- 48 格时段选择器 -->
      <SlotPicker
        v-if="resource"
        v-model:selected="selected"
        :resource="resource"
        :date="date"
        :occupied="slots.occupied"
        :occupied-labels="slots.occupiedLabels"
        :available="slots.available"
        :total-slot-count="slots.totalSlotCount"
        :slot-size-minutes="slots.slotSizeMinutes"
        :loading="slotsLoading"
        :submitting="submitting"
        :disabled="!bookable || noSlotLeft"
        :action-text="noSlotLeft ? '加入候补' : '立即预约'"
        @submit="onSubmit"
      />
    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { showConfirmDialog, showDialog, showFailToast, showSuccessToast, showToast } from 'vant'

import StatusTag from '../components/StatusTag.vue'
import SlotPicker from '../components/SlotPicker.vue'
import { CAPACITY_UNIT, RESOURCE_EMOJI } from '../constants/enums'
import { getResource, getSlots } from '../api/resource'
import { createReservation } from '../api/reservation'
import { joinWaitlist } from '../api/waitlist'
import { formatMoney } from '../utils/format'
import { nextDays } from '../utils/datetime'
import { useDictStore } from '../stores/dict'

const route = useRoute()
const router = useRouter()
const dict = useDictStore()

const days = nextDays(7)
const date = ref(days[0].date)

const resource = ref(null)
const selected = ref([])
const submitting = ref(false)

const slotsLoading = ref(false)
const slots = reactive({
  occupied: [],
  occupiedLabels: [],
  available: null,
  totalSlotCount: 48,
  slotSizeMinutes: 30,
})

const emoji = computed(() => RESOURCE_EMOJI[resource.value?.type] || '📦')
const capacityUnit = computed(() => CAPACITY_UNIT[resource.value?.type] || '人')

const typeLabel = computed(() => {
  const t = resource.value?.type
  if (resource.value?.typeLabel) return resource.value.typeLabel
  const hit = dict.types.find((x) => x.value === t)
  return hit?.label || t || ''
})

/** 可约 = FREE / PENDING_REVIEW / LOCKED（与后端 isUnbookableState 口径一致） */
const bookable = computed(() => {
  const s = resource.value?.status
  return s === 'FREE' || s === 'PENDING_REVIEW' || s === 'LOCKED'
})

/** 当天是否已无空位（用于把主按钮切成「加入候补」） */
const noSlotLeft = computed(() => {
  const total = slots.totalSlotCount || 48
  if (!total) return true
  const avail = slots.available
  if (Array.isArray(avail)) return avail.length === 0
  return slots.occupied.length >= total
})

async function loadResource() {
  const id = Number(route.params.id)
  if (!Number.isFinite(id) || id <= 0) {
    showFailToast('资源不存在')
    router.replace({ name: 'home' })
    return
  }
  try {
    resource.value = await getResource(id)
  } catch {
    router.replace({ name: 'home' })
  }
}

async function loadSlots() {
  if (!resource.value) return
  slotsLoading.value = true
  try {
    const data = await getSlots(resource.value.id, date.value)
    slots.occupied = data?.occupied || []
    slots.occupiedLabels = data?.occupiedLabels || []
    slots.available = data?.available || []
    slots.totalSlotCount = data?.totalSlotCount || 48
    slots.slotSizeMinutes = data?.slotSizeMinutes || 30
  } catch {
    slots.occupied = []
    slots.available = []
  } finally {
    slotsLoading.value = false
  }
}

function pickDate(d) {
  if (d === date.value) return
  date.value = d
  selected.value = []
  loadSlots()
}

function goBack() {
  if (window.history.length > 1) router.back()
  else router.replace({ name: 'home' })
}

/** 提交前确认：资源 / 时段 / 押金 / 总时长 */
async function onSubmit(payload) {
  if (noSlotLeft.value) {
    await onJoinWaitlist(payload)
    return
  }

  const hours = ((payload.endIndex - payload.startIndex + 1) * slots.slotSizeMinutes) / 60
  const deposit = Number(resource.value?.depositAmount || 0)

  try {
    await showConfirmDialog({
      title: '确认预约',
      message:
        `资源：${resource.value.name}（${resource.value.code}）\n` +
        `时段：${date.value} ${payload.startTime.slice(11, 16)}–${payload.endTime.slice(11, 16)}（${hours} 小时）\n` +
        `押金：${deposit > 0 ? `¥${deposit.toFixed(2)}（预约时冻结）` : '免费'}`,
      confirmButtonText: '确认预约',
      cancelButtonText: '再想想',
    })
  } catch {
    return // 用户取消
  }

  submitting.value = true
  try {
    const res = await createReservation({
      resourceId: resource.value.id,
      startTime: payload.startTime,
      endTime: payload.endTime,
    })

    // 需审核资源：必须用 Dialog（Toast 一闪而过会让人以为直接约上了）
    if (res?.status === 'PENDING_REVIEW') {
      await showDialog({
        title: '已提交，等待审核',
        message:
          `预约单 ${res.reservationNo} 已提交。\n\n该资源需要管理员审核，审核通过后会锁定时段；` +
          `审核结果可在「我的预约」查看。`,
        confirmButtonText: '查看我的预约',
      })
    } else {
      showSuccessToast('预约成功')
    }
    router.replace({ name: 'reservations' })
  } catch (e) {
    await handleCreateError(e, payload)
  } finally {
    submitting.value = false
  }
}

/** 按 http.js 的 SPECIAL_CODE 分流处理下单失败 */
async function handleCreateError(e, payload) {
  const code = e?.code
  if (code === 3005) {
    // 时段被占：重新拉 slots（冲突格会以「已占」红色呈现）
    await loadSlots()
    await showDialog({
      title: '时段已被占用',
      message: '你选的时段刚刚被别人订走了，已为你刷新最新占用情况，请重新选择。',
      confirmButtonText: '重新选择',
    })
    selected.value = []
    return
  }
  if (code === 3010 || code === 3011) {
    try {
      await showDialog({
        title: code === 3010 ? '账号处于黑名单' : '信用分不足',
        message:
          code === 3010
            ? '你的账号处于黑名单，暂时无法预约。可在「我的信用」查看扣分记录与恢复方式。'
            : '你的信用分低于预约门槛，暂时无法预约。可在「我的信用」查看明细。',
        confirmButtonText: '查看我的信用',
        cancelButtonText: '知道了',
        showCancelButton: true,
      })
      router.push({ name: 'credit' })
    } catch {
      /* 点「知道了」 */
    }
    return
  }
  if (code === 3012) {
    showFailToast('该资源当前不可预约')
    return
  }
  if (code === 3009) {
    showFailToast('预约时间不合法，请重新选择时段')
    return
  }
  // 其余码拦截器已提示
  void payload
}

/** 已无空位 → 加入候补 */
async function onJoinWaitlist(payload) {
  try {
    await showConfirmDialog({
      title: '加入候补',
      message: `当天已无空位，是否加入候补？\n\n时段：${payload.startTime.slice(11, 16)}–${payload.endTime.slice(11, 16)}`,
      confirmButtonText: '加入候补',
    })
  } catch {
    return
  }

  submitting.value = true
  try {
    await joinWaitlist(resource.value.id, {
      startTime: payload.startTime,
      endTime: payload.endTime,
    })
    showSuccessToast('已加入候补')
    router.replace({ name: 'waitlist' })
  } catch {
    /* 3020 已在队列 / 其他码拦截器已提示 */
  } finally {
    submitting.value = false
  }
}

onMounted(async () => {
  dict.ensureLoaded().catch(() => {})
  // 支持 ?date= 预置日期（从别处跳过来）
  const q = route.query.date
  if (typeof q === 'string' && days.some((d) => d.date === q)) date.value = q

  await loadResource()
  await loadSlots()
})
</script>

<style scoped>
.rf-date-bar {
  display: flex;
  gap: 8px;
  overflow-x: auto;
  padding: 8px 0 10px;
  -webkit-overflow-scrolling: touch;
}

.rf-date-item {
  flex: 0 0 auto;
  min-width: 62px;
  padding: 6px 10px;
  text-align: center;
  background: #fff;
  border: 1px solid #ebedf0;
  border-radius: 8px;
  color: var(--rf-text);
}

.rf-date-item.is-active {
  background: var(--rf-primary);
  border-color: var(--rf-primary);
  color: #fff;
}

.rf-date-label {
  font-size: 13px;
  font-weight: 600;
}

.rf-date-week {
  font-size: 11px;
  opacity: 0.75;
}
</style>
