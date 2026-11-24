// =============================================================================
// src/utils/datetime.js  —  时间解析与倒计时（ARCH-P5 附录 B / D-P5-9）
//
// 与 format.js 的分工：format.js 只做**字符串切片**（展示用）；
// 本文件需要真做时间运算（倒计时、判断槽位是否已过），所以必须解析成 Date。
//
// ⚠️ 解析必须用**本地时区构造**（new Date(y, m-1, d, h, mi, s)），
//    不能用 new Date(isoString)：后者把无时区后缀的串按 UTC 解析，
//    东八区会整体偏移 8 小时（这是 P4 就踩过的坑，D-P4-9）。
// =============================================================================

/**
 * "yyyy-MM-ddTHH:mm:ss" → 本地 Date
 * @param {string} iso
 * @returns {Date|null}
 */
export function parseLocalDateTime(iso) {
  if (!iso) return null
  const m = String(iso).match(/^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})(?::(\d{2}))?/)
  if (!m) return null
  return new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]),
    Number(m[6] || 0))
}

/**
 * 倒计时文本
 * @param {string} expiresAtIso 后端给的截止时间（本地时间串）
 * @param {number} [nowMs] 当前时间戳（单测可注入）
 * @returns {string} "14:59" / "00:30" / "已过期"
 */
export function countdownText(expiresAtIso, nowMs = Date.now()) {
  const end = parseLocalDateTime(expiresAtIso)
  if (!end) return ''
  const left = Math.floor((end.getTime() - nowMs) / 1000)
  if (left <= 0) return '已过期'
  const mm = Math.floor(left / 60)
  const ss = left % 60
  return `${String(mm).padStart(2, '0')}:${String(ss).padStart(2, '0')}`
}

/**
 * 倒计时剩余秒数（负数表示已过期）
 * @param {string} expiresAtIso
 * @param {number} [nowMs]
 */
export function secondsLeft(expiresAtIso, nowMs = Date.now()) {
  const end = parseLocalDateTime(expiresAtIso)
  if (!end) return 0
  return Math.floor((end.getTime() - nowMs) / 1000)
}

/**
 * 某天的第 index 个槽位是否已经过去（用于时段选择器置灰）
 * @param {string} dateStr "yyyy-MM-dd"
 * @param {number} index 0..47
 * @param {Date} [now]
 */
export function isPastSlot(dateStr, index, now = new Date()) {
  const start = parseLocalDateTime(`${dateStr}T${String(Math.floor(index / 2)).padStart(2, '0')}:${
    index % 2 === 0 ? '00' : '30'
  }:00`)
  if (!start) return true
  return start.getTime() <= now.getTime()
}

/**
 * 今天起 N 天的日期数组（含今天），用于日期 Tab / 日历默认范围
 * @param {number} [days]
 * @returns {{date:string,label:string,weekday:string}[]}
 */
export function nextDays(days = 7) {
  const week = ['周日', '周一', '周二', '周三', '周四', '周五', '周六']
  const out = []
  const base = new Date()
  for (let i = 0; i < days; i += 1) {
    const d = new Date(base.getFullYear(), base.getMonth(), base.getDate() + i)
    const p = (x) => String(x).padStart(2, '0')
    const date = `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`
    out.push({
      date,
      label: i === 0 ? '今天' : i === 1 ? '明天' : `${p(d.getMonth() + 1)}-${p(d.getDate())}`,
      weekday: week[d.getDay()],
    })
  }
  return out
}
