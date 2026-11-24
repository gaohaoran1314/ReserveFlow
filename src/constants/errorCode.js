// =============================================================================
// src/constants/errorCode.js
// 只维护「需要页面做额外动作」的错误码 + 一句人话提示。
// **不做全量码表**：后端 msg 本来就是可直接展示的中文（P0–P3 的一致约定），
// 前端再抄一份必然与后端漂移（ARCH-P4 §3.3）。
// =============================================================================

/**
 * @typedef {{ action: string, tip: string }} SpecialCode
 * action 取值：
 *   refreshSlots    重新拉占用时间轴（时段被别人抢了）
 *   refreshDetail   重新拉详情（状态已变，按钮要重算）
 *   closeDrawer     关闭详情抽屉（无权操作）
 *   goCredit        跳转到该用户的信用页
 *   refreshResources 重新拉资源列表（资源态变了）
 *   suggestSystem   建议去 /system 看探针（基础设施类故障）
 */
export const SPECIAL_CODE = {
  3004: { action: 'none', tip: '资源正忙，请稍后重试' },
  3005: { action: 'refreshSlots', tip: '所选时段已被占用，已为你刷新占用情况' },
  3007: { action: 'refreshDetail', tip: '预约状态已变化，已为你刷新最新状态' },
  3008: { action: 'closeDrawer', tip: '无权操作该预约' },
  3010: { action: 'goCredit', tip: '该账号处于黑名单' },
  3011: { action: 'goCredit', tip: '该账号信用分不足' },
  3012: { action: 'refreshResources', tip: '资源当前不可预约' },
  3029: { action: 'none', tip: '规则引擎暂时降级，请稍后重试' },
  1009: { action: 'suggestSystem', tip: '数据库访问异常' },
  1010: { action: 'suggestSystem', tip: '缓存访问异常' },
  1011: { action: 'suggestSystem', tip: '后端配置异常' },
}

/**
 * 常见提示文案（仅用于前端本地校验，与后端 msg 保持一致口径）
 */
export const LOCAL_TIP = {
  required: '该项为必填',
  tooLong: (n) => `长度不能超过 ${n} 个字符`,
  notAdmin: '该账号不是管理员，无法进入后台',
}
