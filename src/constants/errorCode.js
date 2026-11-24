// =============================================================================
// src/constants/errorCode.js  —  错误码 → 移动端呈现方式（ARCH-P5 §8）
//
// 只维护「需要页面做额外动作」的码 + 一句人话；全量码表不做（后端 msg 已是
// 可直接展示的中文）。present 决定用 Toast 还是 Dialog：
//   · toast  轻提示（默认，2 秒自动消失，不打断操作）
//   · dialog 需要用户确认/知晓的（如时段被占、状态已变）
// =============================================================================

export const SPECIAL_CODE = {
  3004: { present: 'toast', tip: '资源正忙，请稍后重试' },
  3005: { present: 'dialog', tip: '你选的时段刚刚被别人订走了，已为你刷新可选时段', action: 'refreshSlots' },
  3007: { present: 'dialog', tip: '预约状态已变化，已为你刷新最新状态', action: 'refreshDetail' },
  3008: { present: 'toast', tip: '无权操作该预约' },
  3009: { present: 'toast', tip: '预约时间不合法，请重新选择时段' },
  3010: { present: 'dialog', tip: '你的账号处于黑名单，暂时无法预约', action: 'goCredit' },
  3011: { present: 'dialog', tip: '信用分不足，暂时无法预约', action: 'goCredit' },
  3012: { present: 'toast', tip: '该资源当前不可预约' },
  3015: { present: 'toast', tip: '还没到可取用时间（可提前 15 分钟取用）' },
  3016: { present: 'dialog', tip: '已超过取用时限，该预约将被系统自动取消' },
  3019: { present: 'toast', tip: '当前候补状态不允许该操作' },
  3020: { present: 'toast', tip: '该时段已在你的候补队列中' },
  3021: { present: 'dialog', tip: '候补名额已过期，已为你刷新队列', action: 'refreshWaitlist' },
  3029: { present: 'toast', tip: '规则校验暂不可用，请稍后重试' },
  1009: { present: 'toast', tip: '服务端数据库异常，请稍后重试' },
  1010: { present: 'toast', tip: '服务端缓存异常，请稍后重试' },
}

/** 登录相关的本地文案 */
export const LOCAL_TIP = {
  required: '请填写完整',
  disabled: '该账号已被禁用，请联系管理员',
}
