<template>
  <div class="rf-picker">
    <!-- 两段 Tab：上午 0–23 / 下午 24–47（切 Tab 只换渲染子集，不重置选择） -->
    <van-tabs v-model:active="half" :ellipsis="false" shrink @change="onHalfChange">
      <van-tab title="上午 00:00-11:30" name="am" />
      <van-tab title="下午 12:00-23:30" name="pm" />
    </van-tabs>

    <div class="rf-slot-wrap">
      <div v-if="loading" class="rf-slot-loading">
        <van-loading size="22" vertical>加载时段…</van-loading>
      </div>
      <div class="rf-slot-grid">
        <div
          v-for="idx in visibleSlots"
          :key="idx"
          class="rf-slot-cell"
          :class="cellClass(idx)"
          @click="onCellClick(idx)"
        >
          <span>{{ slotLabel(idx) }}</span>
          <span v-if="stateOf(idx) !== 'free'" class="rf-slot-tip">{{ tipOf(idx) }}</span>
        </div>
      </div>

      <div v-if="!loading && !totalSlotCount" class="rf-empty">
        <div class="rf-empty-icon">🕐</div>
        <div class="rf-empty-text">这一天没有可选时段</div>
      </div>
    </div>

    <!-- 吸底汇总条 -->
    <div v-if="showBar" class="rf-bottom-bar">
      <div class="rf-bottom-info">
        <template v-if="startIndex === null">
          请选择时段
        </template>
        <template v-else>
          <div class="rf-range-text">
            {{ slotLabel(startIndex) }} – {{ slotLabel(endIndex + 1) }}
            <span class="rf-muted">· {{ hoursText }}</span>
          </div>
          <div class="rf-muted">
            押金 {{ depositText }}
          </div>
        </template>
      </div>
      <van-button
        type="primary"
        round
        :disabled="startIndex === null || disabled"
        :loading="submitting"
        @click="onSubmit"
      >
        {{ startIndex === null ? '请选择时段' : actionText }}
      </van-button>
    </div>
  </div>
</template>

<script setup>
import { computed, ref, watch } from 'vue'
import { showToast } from 'vant'

import { slotLabel } from '../utils/format'
import { isPastSlot, parseLocalDateTime } from '../utils/datetime'

/**
 * 时段选择器（ARCH-P5 §6.1 Q1）—— 学生端最核心的交互
 *
 * 三条关键设计：
 *   1) **选中状态存绝对序号 0–47**，切 Tab 只换渲染子集、绝不重置 ——
 *      所以可以「点上午 11:30 → 切下午 → 点 13:00」得到 11:30–13:00 的连续区间。
 *   2) 4 列网格，格子宽度用 calc 自适应（375 视口下约 81.8px）。
 *   3) 区间内若含「已占用 / 已过」的格子 → 截断到合法的一侧并提示，不会静默吞掉。
 *   4) **第二次点击是"结束时刻"**：点 11:30 再点 13:00 = 11:30–13:00（3 格 / 1.5 小时），
 *      不是到 13:30 —— 用户说"到 13:00"就是这个意思。
 */
const props = defineProps({
  /** 资源对象（取押金、类型用于文案） */
  resource: { type: Object, default: null },
  /** 日期 "yyyy-MM-dd" */
  date: { type: String, default: '' },
  /** v-model:selected —— 连续序号数组（升序） */
  selected: { type: Array, default: () => [] },
  /** 后端 slots 接口的原始字段 */
  occupied: { type: Array, default: () => [] },
  occupiedLabels: { type: Array, default: () => [] },
  available: { type: Array, default: null },
  totalSlotCount: { type: Number, default: 48 },
  slotSizeMinutes: { type: Number, default: 30 },
  loading: { type: Boolean, default: false },
  submitting: { type: Boolean, default: false },
  /** 底部主按钮文案 */
  actionText: { type: String, default: '立即预约' },
  /** 外部禁用（如资源不可约） */
  disabled: { type: Boolean, default: false },
  /** 是否显示吸底汇总条 */
  showBar: { type: Boolean, default: true },
})

const emit = defineEmits(['update:selected', 'submit', 'change'])

const half = ref('am')

/** 绝对序号：由 selected 计算首尾（selected 是连续区间） */
const startIndex = computed(() => (props.selected?.length ? props.selected[0] : null))
const endIndex = computed(() =>
  props.selected?.length ? props.selected[props.selected.length - 1] : null
)

const totalSlotCount = computed(() => props.totalSlotCount || 48)

/** 当前 Tab 渲染的子集：上午 0–23，下午 24–47 */
const visibleSlots = computed(() => {
  const all = Array.from({ length: totalSlotCount.value }, (_, i) => i)
  return half.value === 'am' ? all.slice(0, 24) : all.slice(24)
})

const occupiedSet = computed(() => new Set(props.occupied || []))
const availableSet = computed(() =>
  Array.isArray(props.available) ? new Set(props.available) : null
)
const selectedSet = computed(() => new Set(props.selected || []))

/** 时段是否已过去（今天已过的格子置灰） */
function isPast(idx) {
  if (!props.date) return false
  return isPastSlot(props.date, idx)
}

function isBlocked(idx) {
  if (occupiedSet.value.has(idx)) return true
  if (isPast(idx)) return true
  if (availableSet.value && !availableSet.value.has(idx)) return true
  return false
}

/** free / occupied / past / selected */
function stateOf(idx) {
  if (occupiedSet.value.has(idx)) return 'occupied'
  if (isPast(idx)) return 'past'
  if (selectedSet.value.has(idx)) return 'selected'
  return 'free'
}

function tipOf(idx) {
  const s = stateOf(idx)
  if (s === 'occupied') return '已占'
  if (s === 'past') return '已过'
  if (s === 'selected') return '已选'
  return ''
}

function cellClass(idx) {
  const s = stateOf(idx)
  return {
    'is-occupied': s === 'occupied',
    'is-past': s === 'past',
    'is-selected': s === 'selected',
    'is-edge': startIndex.value === idx || endIndex.value === idx,
  }
}

/** 区间内是否夹着不可选格子 */
function firstBlockedIn(lo, hi) {
  for (let i = lo; i <= hi; i += 1) {
    if (isBlocked(i)) return i
  }
  return -1
}

/**
 * 点格子的三种情形：
 *   ① 还没选 → 设为单格
 *   ② 只选了一个点 → 与它组成区间（夹到不可选格就截断）
 *   ③ 已经是区间 → 重新从这一格开始选
 */
function onCellClick(idx) {
  if (isBlocked(idx)) {
    showToast('该时段不可选')
    return
  }

  const s = startIndex.value
  const e = endIndex.value

  // ③ 已有区间：重开
  if (s !== null && e !== null && e > s) {
    applyRange(idx, idx)
    return
  }

  // ① 首次点击
  if (s === null) {
    applyRange(idx, idx)
    return
  }

  // ② 只有一个点时，**第二次点击 = 结束时刻（不含该格）**
  //    依据：点 11:30 再点 13:00，用户期望的是「11:30–13:00 共 3 格」，不是到 13:30。
  const lo = Math.min(s, idx)
  const hi = Math.max(s, idx) // 结束边界
  const lastSlot = hi - 1 // 区间最后一格（含）

  // 点到相邻格（如 11:30 再点 12:00）→ 就是这一格，不触发重选
  if (lastSlot < lo) {
    return
  }

  const bad = firstBlockedIn(lo, lastSlot)
  if (bad >= 0) {
    // 截断到不含冲突格的一侧（以首次点击的那端为锚点）
    if (idx > s) {
      if (bad - 1 < s) {
        showToast('紧邻的时段不可选，请换个起点')
        return
      }
      applyRange(s, bad - 1)
      showToast(`区间内有不可选时段，已截断到 ${slotLabel(s)}–${slotLabel(bad)}`)
    } else {
      if (bad + 1 > s) {
        showToast('紧邻的时段不可选，请换个起点')
        return
      }
      applyRange(bad + 1, s)
      showToast(`区间内有不可选时段，已截断到 ${slotLabel(bad + 1)}–${slotLabel(s + 1)}`)
    }
    return
  }
  applyRange(lo, lastSlot)
}

/** 把 [lo,hi] 变成连续的序号数组往上抛 */
function applyRange(lo, hi) {
  const arr = []
  for (let i = lo; i <= hi; i += 1) arr.push(i)
  emit('update:selected', arr)
  emit('change', {
    startIndex: lo,
    endIndex: hi,
    startTime: buildTime(lo),
    endTime: buildTime(hi + 1),
  })
}

/** "yyyy-MM-ddTHH:mm:ss"（秒必须为 0，否则后端 3009） */
function buildTime(index) {
  if (!props.date) return ''
  return `${props.date}T${slotLabel(index)}:00`
}

const hoursText = computed(() => {
  if (startIndex.value === null) return ''
  const minutes = (endIndex.value - startIndex.value + 1) * props.slotSizeMinutes
  const h = minutes / 60
  return `${Number.isInteger(h) ? h : h.toFixed(1)} 小时`
})

const depositText = computed(() => {
  const d = Number(props.resource?.depositAmount || 0)
  return d > 0 ? `¥${d.toFixed(2)}` : '免费'
})

function onSubmit() {
  if (startIndex.value === null) {
    showToast('请选择时段')
    return
  }
  emit('submit', {
    startIndex: startIndex.value,
    endIndex: endIndex.value,
    startTime: buildTime(startIndex.value),
    endTime: buildTime(endIndex.value + 1),
  })
}

function onHalfChange() {
  // 切 Tab 只换视图，不动 selection（跨段连续区间靠这个）
}

// 换日期时清空选择（新的一天从零开始）
watch(
  () => props.date,
  () => {
    if (props.selected?.length) emit('update:selected', [])
  }
)

/** 供父组件在 3005 后清空/重置 */
defineExpose({ clear: () => emit('update:selected', []) })
</script>

<style scoped>
.rf-picker {
  padding-bottom: 8px;
}

.rf-slot-wrap {
  padding: 12px;
  position: relative;
  min-height: 120px;
}

.rf-slot-loading {
  position: absolute;
  inset: 0;
  z-index: 2;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(255, 255, 255, 0.75);
}

/* 4 列 × 6 行；格子宽度自适应：容器宽 - 3 个间隙，再除以 4 */
.rf-slot-grid {
  display: grid;
  grid-template-columns: repeat(4, calc((100% - 24px) / 4));
  gap: 8px;
  justify-content: space-between;
}

.rf-slot-cell {
  min-height: 44px;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  border: 1px solid #ebedf0;
  border-radius: 6px;
  background: #fff;
  font-size: 13px;
  color: var(--rf-text);
  user-select: none;
  transition: background 0.15s, border-color 0.15s;
}

.rf-slot-cell .rf-slot-tip {
  font-size: 10px;
  line-height: 1.2;
  margin-top: 1px;
  color: var(--rf-text-2);
}

.rf-slot-cell.is-occupied {
  background: #fff1f0;
  border-color: #ffccc7;
  color: #ee0a24;
}

.rf-slot-cell.is-occupied .rf-slot-tip {
  color: #ee0a24;
}

.rf-slot-cell.is-past {
  background: #f7f8fa;
  border-color: #ebedf0;
  color: #c8c9cc;
}

.rf-slot-cell.is-selected {
  background: #e8f3ff;
  border-color: var(--rf-primary);
  color: var(--rf-primary);
  font-weight: 500;
}

.rf-slot-cell.is-selected .rf-slot-tip {
  color: var(--rf-primary);
}

.rf-slot-cell.is-edge {
  border-width: 2px;
}

.rf-range-text {
  font-size: 14px;
  font-weight: 600;
  color: var(--rf-text);
}
</style>
