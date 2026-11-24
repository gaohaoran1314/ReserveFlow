<template>
  <div>
    <div class="rf-slot-legend">
      <span><i class="rf-dot is-available"></i>可约</span>
      <span><i class="rf-dot is-occupied"></i>已占用</span>
      <span v-if="selectable"><i class="rf-dot is-selected"></i>已选中</span>
      <span class="rf-muted" style="margin-left: auto">
        每格 {{ slotMinutes }} 分钟 · 共 {{ total }} 格
      </span>
    </div>

    <div v-loading="loading" class="rf-slot-grid">
      <el-tooltip
        v-for="i in total"
        :key="i"
        :content="tooltipOf(i - 1)"
        placement="top"
        :show-after="120"
      >
        <div
          class="rf-slot-cell"
          :class="cellClass(i - 1)"
          @click="onClick(i - 1)"
        >
          <span>{{ slotLabel(i - 1) }}</span>
        </div>
      </el-tooltip>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

import { slotLabel } from '../utils/format'

/**
 * 48 格半小时占用轴（ARCH-P4 T07）
 *
 * 数据口径完全来自后端 `GET /resources/{id}/slots?date=`：
 *   `occupied`（序号数组）/ `occupiedLabels`（"09:00-09:30" 字符串数组）
 * 组件不自己算占用——避免与后端唯一的真相源打架。
 *
 * 资源页用来"看"，预约详情里可以用来"选时段"（selectable=true 时点选）。
 */
const props = defineProps({
  /** 已占用序号（后端 occupied） */
  occupied: { type: Array, default: () => [] },
  /** 已占用标签（后端 occupiedLabels），有就用作 tooltip 文案 */
  occupiedLabels: { type: Array, default: () => [] },
  /** 可约序号；不传视为"未占用即可约" */
  available: { type: Array, default: null },
  loading: { type: Boolean, default: false },
  /** 是否允许点选（默认只读） */
  selectable: { type: Boolean, default: false },
  /** 选中序号数组（v-model:selected） */
  selected: { type: Array, default: () => [] },
  totalSlotCount: { type: Number, default: 48 },
  slotSizeMinutes: { type: Number, default: 30 },
})

const emit = defineEmits(['update:selected', 'pick'])

const total = computed(() => props.totalSlotCount || 48)
const slotMinutes = computed(() => props.slotSizeMinutes || 30)

const occupiedSet = computed(() => new Set(props.occupied || []))
const availableSet = computed(() =>
  Array.isArray(props.available) ? new Set(props.available) : null
)
const selectedSet = computed(() => new Set(props.selected || []))

function isOccupied(i) {
  return occupiedSet.value.has(i)
}

function cellClass(i) {
  if (isOccupied(i)) return 'is-occupied'
  if (selectedSet.value.has(i)) return 'is-selected'
  return ''
}

function tooltipOf(i) {
  const label = `${slotLabel(i)}-${slotLabel(i + 1)}`
  if (isOccupied(i)) {
    const idx = (props.occupied || []).indexOf(i)
    const detail = idx >= 0 ? props.occupiedLabels?.[idx] : ''
    return `${label} 已占用${detail ? `（${detail}）` : ''}`
  }
  return `${label} 可约`
}

function onClick(i) {
  if (!props.selectable) return
  if (isOccupied(i)) return
  if (availableSet.value && !availableSet.value.has(i)) return

  const next = new Set(props.selected || [])
  if (next.has(i)) next.delete(i)
  else next.add(i)

  const arr = [...next].sort((a, b) => a - b)
  emit('update:selected', arr)
  emit('pick', i, arr)
}
</script>

<style scoped>
.rf-slot-legend {
  display: flex;
  align-items: center;
  gap: 14px;
  font-size: 12px;
  color: var(--rf-text-secondary);
  margin-bottom: 8px;
}

.rf-dot {
  display: inline-block;
  width: 10px;
  height: 10px;
  border-radius: 2px;
  margin-right: 4px;
  vertical-align: -1px;
  border: 1px solid var(--rf-border);
}

.rf-dot.is-available {
  background: #fff;
}

.rf-dot.is-occupied {
  background: #fef0f0;
  border-color: #fbc4c4;
}

.rf-dot.is-selected {
  background: #ecf5ff;
  border-color: #409eff;
}
</style>
