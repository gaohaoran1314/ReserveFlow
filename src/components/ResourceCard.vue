<template>
  <div class="rf-card" :class="{ 'is-disabled': disabled }" @click="onClick">
    <div class="rf-card-head">
      <span class="rf-emoji">{{ emoji }}</span>
      <div class="rf-grow" style="min-width: 0">
        <div class="rf-card-title">{{ item.name }}</div>
        <div class="rf-card-sub rf-mono">{{ item.code }}</div>
      </div>
      <StatusTag
        type="resource"
        :value="item.status"
        :label="item.statusLabel"
      />
    </div>

    <div class="rf-card-sub">
      📍 {{ item.location || '未填写地点' }} · 可容纳 {{ item.capacity }}{{ capacityUnit }}
    </div>

    <div class="rf-card-foot">
      <span :class="{ 'rf-muted': !item.depositAmount }">
        {{ item.depositAmount > 0 ? `押金 ¥${formatMoney(item.depositAmount)}` : '免费' }}
      </span>
      <span class="rf-muted" style="font-size: 12px">
        {{ disabled ? '暂不可预约' : '查看详情 ›' }}
      </span>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

import StatusTag from './StatusTag.vue'
import { CAPACITY_UNIT, RESOURCE_EMOJI } from '../constants/enums'
import { formatMoney } from '../utils/format'

/**
 * 资源卡片（首页，ARCH-P5 §5.2）
 * 维护中/停用/删除的也展示，但置灰且不跳详情（后端也会在 3012 拒绝）。
 */
const props = defineProps({
  item: { type: Object, required: true },
})

const emit = defineEmits(['click'])

const emoji = computed(() => RESOURCE_EMOJI[props.item.type] || '📦')
const capacityUnit = computed(() => CAPACITY_UNIT[props.item.type] || '人')

/** 是否不可约：与后端 isUnbookableState 的口径一致（FREE/PENDING_REVIEW/LOCKED 可约） */
const disabled = computed(() => {
  const s = props.item.status
  return !(s === 'FREE' || s === 'PENDING_REVIEW' || s === 'LOCKED')
})

function onClick() {
  if (disabled.value) return
  emit('click', props.item)
}
</script>
