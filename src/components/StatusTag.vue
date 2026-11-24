<template>
  <van-tag :type="tagType" :plain="plain" size="medium" :text-color="textColor">
    {{ text }}
  </van-tag>
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
 * 状态标签（Vant 版，ARCH-P5 §4）
 *
 * 双轨：资源与预约**优先用后端给的 label**（props.label），本组件只决定颜色；
 * 押金/用户状态后端不给中文，用本地枚举兜底。
 */
const props = defineProps({
  /** reservation | deposit | credit | user | resource | waitlist */
  type: { type: String, default: 'reservation' },
  value: { type: String, default: '' },
  label: { type: String, default: '' },
  plain: { type: Boolean, default: false },
})

/** 本地枚举里的 tagType → Vant 的 type（default 用无 type 的灰标签） */
function toVantTag(t) {
  if (!t || t === 'default') return ''
  return t
}

const RESOURCE_TAG = {
  FREE: 'success',
  PENDING_REVIEW: 'warning',
  LOCKED: 'primary',
  IN_USE: 'success',
  DUE_RETURN: 'danger',
  RETURN_CHECK: 'warning',
  MAINTENANCE: 'primary',
  DISABLED: 'default',
  DELETED: 'default',
  DAMAGE_CLAIM: 'danger',
}

/** 候补 5 态的颜色（后端只给 statusLabel 不给颜色） */
const WAITLIST_TAG = {
  WAITING: 'primary',
  OFFERED: 'warning',
  CONVERTED: 'success',
  CANCELLED: 'default',
  EXPIRED: 'default',
}

const RESOURCE_LABEL = {
  FREE: '可用',
  PENDING_REVIEW: '待审核',
  LOCKED: '已锁定',
  IN_USE: '使用中',
  DUE_RETURN: '待归还',
  RETURN_CHECK: '验收中',
  MAINTENANCE: '维护中',
  DISABLED: '已停用',
  DELETED: '已删除',
  DAMAGE_CLAIM: '理赔中',
}

const tagType = computed(() => {
  const v = props.value || ''
  if (props.type === 'reservation') return toVantTag(tagTypeOf(RESERVATION_STATUS, v))
  if (props.type === 'deposit') return toVantTag(tagTypeOf(DEPOSIT_STATE, v))
  if (props.type === 'user') return toVantTag(tagTypeOf(USER_STATUS, v))
  if (props.type === 'resource') return toVantTag(RESOURCE_TAG[v] || 'default')
  if (props.type === 'waitlist') return toVantTag(WAITLIST_TAG[v] || 'default')
  return ''
})

const text = computed(() => {
  const v = props.value || ''
  if (props.label) return props.label
  if (props.type === 'reservation') return labelOf(RESERVATION_STATUS, v)
  if (props.type === 'deposit') return labelOf(DEPOSIT_STATE, v)
  if (props.type === 'user') return labelOf(USER_STATUS, v)
  if (props.type === 'resource') return RESOURCE_LABEL[v] || v || '—'
  return v || '—'
})

/** 维护中/停用的标签用灰字，避免误以为可约 */
const textColor = computed(() => (tagType.value === '' ? '#969799' : undefined))
</script>
