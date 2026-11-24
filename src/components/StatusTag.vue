<template>
  <el-tag :type="tagType" size="small" :effect="effect" disable-transitions>
    {{ text }}
  </el-tag>
</template>

<script setup>
import { computed } from 'vue'

import {
  DEPOSIT_STATE,
  RESERVATION_STATUS,
  USER_STATUS,
  labelOf,
  tagTypeOf,
} from '../constants/enums'

/**
 * 状态标签（ARCH-P4 §5）
 * 覆盖：资源 10 态 / 预约 8 态 / 押金 4 态 / 用户 3 态。
 *
 * 设计要点（D-P4-5 双轨）：
 *   · 资源与预约**优先用后端给的 label**（props.label），本组件只决定颜色；
 *   · 押金 / 用户状态后端不给中文，用本地枚举兜底。
 */
const props = defineProps({
  /** resource | reservation | deposit | user */
  type: { type: String, default: 'reservation' },
  /** 英文枚举值 */
  value: { type: String, default: '' },
  /** 后端返回的中文（有就用它，没有则本地兜底） */
  label: { type: String, default: '' },
  effect: { type: String, default: 'light' },
})

/** 资源 10 态的颜色（后端只给 label 不给颜色，这里按语义定） */
const RESOURCE_TAG = {
  FREE: 'success',
  PENDING_REVIEW: 'warning',
  LOCKED: 'primary',
  IN_USE: 'success',
  DUE_RETURN: 'danger',
  RETURN_CHECK: 'warning',
  MAINTENANCE: 'info',
  DISABLED: 'info',
  DELETED: 'info',
  DAMAGE_CLAIM: 'danger',
}

/** 资源 10 态的中文兜底（正常路径用后端 statusLabel） */
const RESOURCE_LABEL = {
  FREE: '可用',
  PENDING_REVIEW: '待审核',
  LOCKED: '已锁定',
  IN_USE: '使用中',
  DUE_RETURN: '待归还',
  RETURN_CHECK: '归还验收中',
  MAINTENANCE: '维护中',
  DISABLED: '已停用',
  DELETED: '已删除',
  DAMAGE_CLAIM: '理赔中',
}

const tagType = computed(() => {
  const v = props.value || ''
  if (props.type === 'resource') return RESOURCE_TAG[v] || 'info'
  if (props.type === 'reservation') return tagTypeOf(RESERVATION_STATUS, v)
  if (props.type === 'deposit') return tagTypeOf(DEPOSIT_STATE, v)
  if (props.type === 'user') return tagTypeOf(USER_STATUS, v)
  return 'info'
})

const text = computed(() => {
  const v = props.value || ''
  if (props.label) return props.label
  if (props.type === 'resource') return RESOURCE_LABEL[v] || v || '—'
  if (props.type === 'reservation') return labelOf(RESERVATION_STATUS, v)
  if (props.type === 'deposit') return labelOf(DEPOSIT_STATE, v)
  if (props.type === 'user') return labelOf(USER_STATUS, v)
  return v || '—'
})
</script>
