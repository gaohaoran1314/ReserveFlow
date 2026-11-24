<template>
  <div class="rf-card" @click="$emit('detail', item)">
    <div class="rf-card-head">
      <span class="rf-emoji">{{ emoji }}</span>
      <div class="rf-grow" style="min-width: 0">
        <div class="rf-card-title">{{ item.resourceName || '资源' }}</div>
        <div class="rf-card-sub rf-mono">
          {{ item.reservationNo }} · {{ item.resourceCode }}
        </div>
      </div>
      <StatusTag type="reservation" :value="item.status" :label="item.statusLabel" />
    </div>

    <div class="rf-card-sub">
      🕐 {{ formatRange(item.startTime, item.endTime) }}
      <span v-if="item.needReview" style="margin-left: 6px; color: #ff976a">需审核</span>
    </div>

    <!-- 押金是条件输出字段（D-P4-8）：可能缺失，用可选链 -->
    <div v-if="item.deposit" class="rf-card-sub">
      💰 押金
      <StatusTag type="deposit" :value="item.deposit.state" plain />
      <span class="rf-mono" style="margin-left: 4px">¥{{ formatMoney(item.deposit.frozen) }}</span>
    </div>

    <div class="rf-card-foot">
      <span class="rf-muted" style="font-size: 12px">下单于 {{ formatTime(item.createdAt, false) }}</span>
      <div class="rf-row" @click.stop>
        <van-button v-if="action" size="mini" :type="actionType" @click="$emit('action', action.code, item)">
          {{ action.label }}
        </van-button>
        <van-button size="mini" plain type="primary" @click="$emit('detail', item)">详情</van-button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

import StatusTag from './StatusTag.vue'
import { RESERVATION_STATUS_ACTIONS, RESOURCE_EMOJI } from '../constants/enums'
import { formatMoney, formatRange, formatTime } from '../utils/format'

/**
 * 预约卡片（我的预约，ARCH-P5 §4 / §5.4）
 *
 * 主操作按钮**按 status 推导**（列表接口不返回 allowedActions，主理人决策 1）；
 * 点详情后一律以服务端的 allowedActions 为准。
 */
const props = defineProps({
  item: { type: Object, required: true },
})

defineEmits(['detail', 'action'])

const emoji = computed(() => RESOURCE_EMOJI[props.item.resourceType] || '📦')

const action = computed(() => RESERVATION_STATUS_ACTIONS[props.item.status] || null)

const actionType = computed(() => {
  const code = action.value?.code
  if (code === 'CANCEL') return 'danger'
  return 'primary'
})
</script>
