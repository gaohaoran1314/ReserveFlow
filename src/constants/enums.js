// =============================================================================
// src/constants/enums.js  —  前端枚举镜像（ARCH-P4 §6.0 / 附录 B）
//
// 双轨原则（D-P4-5）：
//   · 能拿后端 label 的一律用后端（资源 typeLabel/statusLabel、meta.types/statuses、
//     预约 statusLabel、信用 reasonLabel）——展示层不要用本文件的 label；
//   · 本文件只服务两类场景：**筛选下拉的选项**（后端没有字典接口）、
//     **后端不给中文的枚举**（押金/违约/规则）。
//
// 每个数组上方标注来源；后端调整枚举时同步这里（无运行时校验，后端才是权威）。
// =============================================================================

/** 来源：后端 Model.h toStr(ReservationStatus)（8 态） */
export const RESERVATION_STATUS = [
  { value: 'PENDING_REVIEW', label: '待审核', tagType: 'warning' },
  { value: 'LOCKED', label: '已锁定', tagType: 'primary' },
  { value: 'IN_USE', label: '使用中', tagType: 'success' },
  { value: 'DUE_RETURN', label: '待归还', tagType: 'danger' },
  { value: 'RETURN_CHECK', label: '归还验收中', tagType: 'warning' },
  { value: 'COMPLETED', label: '已完成', tagType: 'info' },
  { value: 'CANCELLED', label: '已取消', tagType: 'info' },
  { value: 'REJECTED', label: '已驳回', tagType: 'info' },
]

/** 来源：后端 credit_log.reason_type（7 值）；展示优先用后端 reasonLabel，本表用于筛选下拉 */
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
  { value: 'NONE', label: '无押金', tagType: 'info' },
  { value: 'FROZEN', label: '已冻结', tagType: 'warning' },
  { value: 'PARTIAL', label: '部分结算', tagType: 'primary' },
  { value: 'SETTLED', label: '已结清', tagType: 'success' },
]

/** 来源：后端 violation_record.violation_type（3 值） */
export const VIOLATION_TYPE = [
  { value: 'OVERDUE', label: '逾期归还', tagType: 'warning' },
  { value: 'NO_SHOW', label: '爽约未取用', tagType: 'danger' },
  { value: 'DAMAGE', label: '损坏赔偿', tagType: 'danger' },
]

/** 来源：后端 rule_config.eval_phase（3 值，ARCH-P3 §3 / D-P3-2） */
export const EVAL_PHASE = [
  { value: 'CREATE', label: '下单前', tagType: 'primary' },
  { value: 'TIMER', label: '定时扫描', tagType: 'warning' },
  { value: 'SETTLE', label: '结算时', tagType: 'success' },
]

/** 来源：后端 rule_config.rule_type（3 值，仅业务分类展示） */
export const RULE_TYPE = [
  { value: 'CREDIT', label: '信用' },
  { value: 'RESERVATION', label: '预约' },
  { value: 'DEPOSIT', label: '押金' },
]

/** 来源：后端 action_json.type（6 值，ARCH-P3 §3.3） */
export const ACTION_TYPE = [
  { value: 'reject', label: '拒绝' },
  { value: 'deduct_credit', label: '扣信用分' },
  { value: 'deduct_only', label: '仅扣分' },
  { value: 'transition', label: '状态迁移' },
  { value: 'freeze_deposit', label: '冻结押金' },
  { value: 'deduct_deposit', label: '扣减押金' },
]

/** 来源：后端 user.status（P6 起 5 值） */
export const USER_STATUS = [
  { value: 'NORMAL', label: '正常', tagType: 'success' },
  { value: 'BLACKLIST', label: '黑名单', tagType: 'danger' },
  { value: 'DISABLED', label: '已禁用', tagType: 'info' },
  // P6 新增两态（后端 UserStatus 已扩到 5 值）：
  // 待审核=橙色（要人处理）、已拒绝=灰（等学生自己重提），与正常/黑名单有明显区分
  { value: 'PENDING', label: '待审核', tagType: 'warning' },
  { value: 'REJECTED', label: '已拒绝', tagType: 'info' },
]

/** 来源：后端 user.role（2 值） */
export const USER_ROLE = [
  { value: 'ADMIN', label: '管理员', tagType: 'danger' },
  { value: 'USER', label: '普通用户', tagType: 'info' },
]

/**
 * 预约动作按钮元数据（ARCH-P4 §7.1）。
 * **可见性完全由后端 allowedActions 驱动**，本表只负责「怎么显示、点了调哪个接口」。
 */
export const ACTION_META = {
  CANCEL: { label: '取消预约', type: 'warning', needReason: false, api: 'cancel' },
  APPROVE: { label: '审核通过', type: 'primary', needReason: false, api: 'approve' },
  REJECT: { label: '驳回', type: 'danger', needReason: true, api: 'reject' },
  START_USE: { label: '取用', type: 'primary', needReason: false, api: 'pickup' },
  SUBMIT_RETURN: { label: '归还', type: 'primary', needReason: false, api: 'returnBack' },
  PASS_CHECK: { label: '验收通过', type: 'success', needReason: false, api: 'accept' },
  MARK_DAMAGE: { label: '损坏结案', type: 'danger', needReason: true, api: 'damage' },
}

/**
 * 兜底映射：仅在 allowedActions **字段缺失**时使用（§7.1 防御 1）。
 * 正常情况下永远以 allowedActions 为准。
 */
export const RESERVATION_STATUS_ACTIONS = {
  PENDING_REVIEW: ['APPROVE', 'REJECT', 'CANCEL'],
  LOCKED: ['START_USE', 'CANCEL'],
  IN_USE: ['SUBMIT_RETURN'],
  DUE_RETURN: ['SUBMIT_RETURN', 'MARK_DAMAGE'],
  RETURN_CHECK: ['PASS_CHECK', 'MARK_DAMAGE'],
}

/**
 * 按 value 找选项；找不到时返回原值（避免界面上出现空白）
 * @param {Array} list
 * @param {string} value
 * @param {string} [fallbackLabel] 找不到时的显示文案，默认原值
 */
export function labelOf(list, value, fallbackLabel) {
  const hit = list.find((x) => x.value === value)
  if (hit) return hit.label
  if (fallbackLabel !== undefined) return fallbackLabel
  return value ?? '—'
}

/**
 * 按 value 找 tag 颜色
 * @param {Array} list
 * @param {string} value
 */
export function tagTypeOf(list, value) {
  const hit = list.find((x) => x.value === value)
  return hit?.tagType || 'info'
}
