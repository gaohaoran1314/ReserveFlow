<template>
  <div>
    <!-- 有 timeline 接口数据：完整流转记录（最新在前） -->
    <template v-if="nodes.length">
      <div v-for="(n, i) in nodes" :key="i" class="rf-timeline-node">
        <div class="rf-timeline-time">{{ formatTime(n.time) }}</div>
        <div>
          <div>
            <el-tag v-if="n.fromLabel" size="small" type="info" effect="plain">{{ n.fromLabel }}</el-tag>
            <el-icon style="margin: 0 4px; vertical-align: middle"><Right /></el-icon>
            <el-tag size="small" :type="n.tagType">{{ n.title }}</el-tag>
            <el-tag v-if="n.ruleCode" size="small" type="warning" effect="plain" style="margin-left: 6px">
              {{ n.ruleCode }}
            </el-tag>
          </div>
          <div v-if="n.desc" class="rf-muted" style="margin-top: 2px; font-size: 12px">
            {{ n.desc }}
          </div>
          <div v-if="n.operator" class="rf-muted" style="font-size: 12px">
            操作人：{{ n.operator }}
          </div>
        </div>
      </div>
    </template>

    <!-- 无 timeline 数据：降级为关键节点摘要（D-P4-7 的降级方案） -->
    <template v-else-if="summary.length">
      <el-alert
        type="info"
        :closable="false"
        show-icon
        title="完整流转记录需状态日志接口，当前为关键节点摘要"
        style="margin-bottom: 8px"
      />
      <div v-for="(n, i) in summary" :key="i" class="rf-timeline-node">
        <div class="rf-timeline-time">{{ formatTime(n.time) }}</div>
        <div>
          <el-tag size="small" :type="n.tagType">{{ n.title }}</el-tag>
          <div v-if="n.desc" class="rf-muted" style="margin-top: 2px; font-size: 12px">
            {{ n.desc }}
          </div>
        </div>
      </div>
    </template>

    <EmptyState v-else title="暂无记录" desc="该预约还没有产生任何状态变更" icon="Clock" />
  </div>
</template>

<script setup>
import { computed } from 'vue'

import EmptyState from './EmptyState.vue'
import { RESERVATION_STATUS, labelOf, tagTypeOf } from '../constants/enums'
import { formatTime } from '../utils/format'

/**
 * 预约状态时间线（ARCH-P4 §7.3）
 *
 * 两种数据来源，页面代码零差异：
 *   1) 传了 `timeline`（GET /reservations/{id}/timeline 的 data.list）→ 完整记录
 *   2) 没传/为空 → 用 createdAt + 押金事件 + updatedAt 降级成关键节点摘要
 */
const props = defineProps({
  /** 接口返回的 list（最新在前）；可为空 */
  timeline: { type: Array, default: () => [] },
  /** 预约详情对象（降级用） */
  reservation: { type: Object, default: null },
  /** 押金流水（降级用，可选） */
  deposits: { type: Array, default: () => [] },
})

/** 完整时间线 */
const nodes = computed(() => {
  if (!Array.isArray(props.timeline) || props.timeline.length === 0) return []
  return props.timeline.map((x) => ({
    time: x.createdAt,
    fromLabel: x.fromStatus ? labelOf(RESERVATION_STATUS, x.fromStatus) : '创建',
    title: labelOf(RESERVATION_STATUS, x.toStatus),
    tagType: tagTypeOf(RESERVATION_STATUS, x.toStatus),
    desc: x.reason || '',
    ruleCode: x.ruleCode || '',
    operator: x.operatorId ? `#${x.operatorId}` : '系统',
  }))
})

/** 降级摘要 */
const summary = computed(() => {
  const r = props.reservation
  if (!r) return []
  const out = []
  if (r.createdAt) {
    out.push({ time: r.createdAt, title: '提交预约', tagType: 'warning', desc: r.resourceName || '' })
  }
  for (const d of props.deposits || []) {
    out.push({
      time: d.createdAt,
      title: `押金${d.recordType === 'FREEZE' ? '冻结' : d.recordType === 'DEDUCT' ? '扣减' : '退还'} ¥${Number(
        d.amount || 0
      ).toFixed(2)}`,
      tagType: d.recordType === 'DEDUCT' ? 'danger' : d.recordType === 'REFUND' ? 'success' : 'info',
      desc: d.remark || '',
    })
  }
  if (r.updatedAt) {
    out.push({
      time: r.updatedAt,
      title: r.statusLabel || labelOf(RESERVATION_STATUS, r.status),
      tagType: tagTypeOf(RESERVATION_STATUS, r.status),
      desc: '当前状态',
    })
  }
  // 时间正序展示（摘要按发生顺序读）
  return out.sort((a, b) => String(a.time).localeCompare(String(b.time)))
})
</script>
