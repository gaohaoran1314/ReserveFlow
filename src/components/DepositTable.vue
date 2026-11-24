<template>
  <div>
    <!-- 账目汇总条：没有押金的资源直接说人话，而不是给一张空表格 -->
    <div v-if="isEmpty" class="rf-muted" style="padding: 8px 0">
      该资源无押金，没有产生任何押金流水。
    </div>

    <template v-else>
      <el-descriptions :column="4" border size="small" style="margin-bottom: 10px">
        <el-descriptions-item label="已冻结">¥{{ formatMoney(account?.frozen) }}</el-descriptions-item>
        <el-descriptions-item label="已扣减">¥{{ formatMoney(account?.deducted) }}</el-descriptions-item>
        <el-descriptions-item label="已退还">¥{{ formatMoney(account?.refunded) }}</el-descriptions-item>
        <el-descriptions-item label="结算状态">
          <StatusTag type="deposit" :value="account?.state" />
        </el-descriptions-item>
      </el-descriptions>

      <el-table :data="list" border size="small" empty-text="暂无流水">
        <el-table-column label="类型" width="90">
          <template #default="{ row }">
            <el-tag size="small" :type="recordTagType(row.recordType)">
              {{ labelOf(DEPOSIT_RECORD_TYPE, row.recordType) }}
            </el-tag>
          </template>
        </el-table-column>
        <el-table-column label="金额" width="110" align="right">
          <template #default="{ row }">
            <span :style="{ color: recordColor(row.recordType) }">
              ¥{{ formatMoney(row.amount) }}
            </span>
          </template>
        </el-table-column>
        <el-table-column prop="transactionNo" label="交易号" min-width="160">
          <template #default="{ row }">
            <span class="rf-mono">{{ row.transactionNo }}</span>
          </template>
        </el-table-column>
        <el-table-column label="时间" width="170">
          <template #default="{ row }">
            <span class="rf-mono">{{ formatTime(row.createdAt) }}</span>
          </template>
        </el-table-column>
        <el-table-column prop="remark" label="说明" min-width="160" show-overflow-tooltip />
      </el-table>
    </template>
  </div>
</template>

<script setup>
import { computed } from 'vue'

import StatusTag from './StatusTag.vue'
import { DEPOSIT_RECORD_TYPE, labelOf, tagTypeOf } from '../constants/enums'
import { formatMoney, formatTime } from '../utils/format'

/**
 * 押金流水表 + 账目汇总（ARCH-P4 §5 / T08）
 *
 * 关键点（D-P4-8）：`deposit` 是**条件输出**字段——零押金资源没有流水也没有
 * `account`，此时显示「该资源无押金」而不是一张空表格。
 */
const props = defineProps({
  /** GET /reservations/{id}/deposits 的 data.account */
  account: { type: Object, default: null },
  /** 同上接口的 data.list */
  list: { type: Array, default: () => [] },
})

const isEmpty = computed(() => {
  const noList = !props.list || props.list.length === 0
  const noFrozen = !props.account || Number(props.account.frozen || 0) === 0
  return noList && noFrozen
})

function recordTagType(t) {
  return tagTypeOf(DEPOSIT_RECORD_TYPE, t)
}

function recordColor(t) {
  if (t === 'DEDUCT') return '#f56c6c'
  if (t === 'REFUND') return '#67c23a'
  return '#e6a23c'
}
</script>
