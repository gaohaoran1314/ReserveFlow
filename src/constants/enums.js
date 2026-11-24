// =============================================================================
// src/constants/enums.js  —  学生端枚举镜像（ARCH-P5 §6.3）
//
// 只保留学生端真正用到的 5 组。刻意**不复制** EVAL_PHASE / RULE_TYPE /
// ACTION_TYPE / USER_ROLE：H5 没有规则页与用户管理页，多复制一份就多一处
// 与后端同步的负担（admin 那份已经要维护了）。
//
// 双轨原则（同 P4）：能拿后端 label 的一律用后端（资源 typeLabel/statusLabel、
// meta.types/statuses、预约 statusLabel、信用 reasonLabel）；本文件只服务
// 「筛选下拉的选项」和「后端不给中文的枚举」。
// =============================================================================

/** 来源：后端 Model.h toStr(ReservationStatus)（8 态） */
export const RESERVATION_STATUS = [
  { value: 'PENDING_REVIEW', label: '待审核', tagType: 'warning' },
  { value: 'LOCKED', label: '已锁定', tagType: 'primary' },
  { value: 'IN_USE', label: '使用中', tagType: 'success' },
  { value: 'DUE_RETURN', label: '待归还', tagType: 'danger' },
  { value: 'RETURN_CHECK', label: '归还验收中', tagType: 'warning' },
  { value: 'COMPLETED', label: '已完成', tagType: 'default' },
  { value: 'CANCELLED', label: '已取消', tagType: 'default' },
  { value: 'REJECTED', label: '已驳回', tagType: 'default' },
]

/** 来源：后端 credit_log.reason_type（7 值）；展示用后端 reasonLabel，本表只用于筛选 */
export const CREDIT_REASON = [
  { value: 'NO_SHOW', label: '爽约未取用' },
  { value: 'OVERDUE', label: '逾期未还' },
  { value: 'DAMAGE', label: '损坏赔偿' },
  { value: 'MANUAL', label: '管理员调整' },
  { value: 'AUTO_BLACKLIST', label: '自动进入黑名单' },
  { value: 'AUTO_RECOVER', label: '自动解除黑名单' },
  { value: 'AUTO_REPAIR', label: '定时纠偏' },
]

/** 来源：后端 deposit_record.record_type（3 值，后端不给中文） */
export const DEPOSIT_RECORD_TYPE = [
  { value: 'FREEZE', label: '冻结', tagType: 'warning' },
  { value: 'DEDUCT', label: '扣减', tagType: 'danger' },
  { value: 'REFUND', label: '退还', tagType: 'success' },
]

/** 来源：后端 DepositAccount::state()（4 值，后端只给英文） */
export const DEPOSIT_STATE = [
  { value: 'NONE', label: '无押金', tagType: 'default' },
  { value: 'FROZEN', label: '已冻结', tagType: 'warning' },
  { value: 'PARTIAL', label: '部分结算', tagType: 'primary' },
  { value: 'SETTLED', label: '已结清', tagType: 'success' },
]

/** 来源：后端 user.status（3 值） */
export const USER_STATUS = [
  { value: 'NORMAL', label: '正常', tagType: 'success' },
  { value: 'BLACKLIST', label: '黑名单', tagType: 'danger' },
  { value: 'DISABLED', label: '已禁用', tagType: 'default' },
]

/**
 * 列表页的「主操作按钮」推导表（ARCH-P5 §5.4 / 主理人决策 1）。
 *
 * ⚠️ 这是**已知的接口形状限制**：`toListJson()` 不返回 allowedActions。
 * 学生端只有 3 个动作且与状态一对一，本地映射够用；
 * **点进详情后一律以 allowedActions 为准**，两者不一致时以详情为准。
 */
export const RESERVATION_STATUS_ACTIONS = {
  PENDING_REVIEW: { code: 'CANCEL', label: '取消预约' },
  LOCKED: { code: 'START_USE', label: '立即取用' },
  IN_USE: { code: 'SUBMIT_RETURN', label: '归还' },
  DUE_RETURN: { code: 'SUBMIT_RETURN', label: '归还' },
  RETURN_CHECK: null,
  COMPLETED: null,
  CANCELLED: null,
  REJECTED: null,
}

/** 学生端允许出现的动作（详情里也从 allowedActions 里按这个白名单过滤） */
export const STUDENT_ACTIONS = ['CANCEL', 'START_USE', 'SUBMIT_RETURN']

/** 按 value 找选项 */
export function labelOf(list, value, fallbackLabel) {
  const hit = list.find((x) => x.value === value)
  if (hit) return hit.label
  if (fallbackLabel !== undefined) return fallbackLabel
  return value ?? '—'
}

/** 按 value 找标签颜色（Vant 的 type：primary/success/warning/danger/default） */
export function tagTypeOf(list, value) {
  const hit = list.find((x) => x.value === value)
  return hit?.tagType || 'default'
}

/** 资源类型的 emoji（避免引入图标库） */
export const RESOURCE_EMOJI = {
  LAB_EQUIPMENT: '🔬',
  MEETING_ROOM: '🏢',
  LOCKER: '🗄️',
}

/** 资源容量单位 */
export const CAPACITY_UNIT = {
  LAB_EQUIPMENT: '人',
  MEETING_ROOM: '座',
  LOCKER: '格',
}
