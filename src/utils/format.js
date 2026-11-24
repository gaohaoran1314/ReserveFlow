// =============================================================================
// src/utils/format.js  —  纯展示格式化（无副作用、无请求）
// 关键约束（D-P4-9）：后端给的是**本地时间字符串**（无时区后缀，形如
// "2026-11-22T17:55:39"）。若用 new Date() 解析会被当 UTC，在东八区显示会
// 整体偏移 8 小时——所以这里**只做字符串切片，不做任何日期运算**。
// =============================================================================

/**
 * "2026-11-22T17:55:39" → "2026-11-22 17:55:39"
 * @param {string} iso
 * @param {boolean} [withSeconds=true]
 */
export function formatTime(iso, withSeconds = true) {
  if (!iso) return '—'
  const s = String(iso).replace('T', ' ')
  return withSeconds ? s : s.slice(0, 16)
}

/**
 * 只要 "MM-DD HH:mm"（列表里省宽度用）
 * @param {string} iso
 */
export function formatTimeShort(iso) {
  if (!iso) return '—'
  const s = String(iso).replace('T', ' ')
  return s.slice(5, 16)
}

/**
 * 金额：数字 → "200.00"（KPI/表格里配合 ¥ 使用）
 * @param {number|string} v
 */
export function formatMoney(v) {
  const n = Number(v)
  if (!Number.isFinite(n)) return '0.00'
  return n.toFixed(2)
}

/**
 * 押金展示：0 显示「免费」
 * @param {number|string} v
 */
export function formatDeposit(v) {
  const n = Number(v)
  if (!Number.isFinite(n) || n === 0) return '免费'
  return `¥${n.toFixed(2)}`
}

/**
 * 时段展示：同一天折叠成 "11-22 09:00 ~ 10:00"，跨天则完整显示
 * @param {string} startIso
 * @param {string} endIso
 */
export function formatRange(startIso, endIso) {
  if (!startIso || !endIso) return '—'
  const s = String(startIso).replace('T', ' ')
  const e = String(endIso).replace('T', ' ')
  const sameDay = s.slice(0, 10) === e.slice(0, 10)
  const left = s.slice(5, 16)
  const right = sameDay ? e.slice(11, 16) : e.slice(5, 16)
  return `${left} ~ ${right}`
}

/**
 * 槽位序号 → 时间点（一天 48 格，每格 30 分钟）
 * @param {number} index 0..48（48 表示次日 00:00）
 */
export function slotLabel(index) {
  const i = Number(index)
  if (!Number.isFinite(i) || i < 0) return '—'
  const h = Math.floor(i / 2)
  const m = i % 2 === 0 ? '00' : '30'
  if (h >= 24) return '24:00'
  return `${String(h).padStart(2, '0')}:${m}`
}

/**
 * 连续槽位序号数组 → 时间标签数组（如 [18,19] → ["09:00-09:30","09:30-10:00"]）
 * @param {number[]} indexes
 */
export function slotLabels(indexes) {
  if (!Array.isArray(indexes) || indexes.length === 0) return []
  return indexes.map((i) => `${slotLabel(i)}-${slotLabel(i + 1)}`)
}

/**
 * 槽位区间 → "09:00-10:00"（起止格）
 * @param {number} from
 * @param {number} to
 */
export function slotRangeLabel(from, to) {
  return `${slotLabel(from)}-${slotLabel(to)}`
}

/**
 * 金额/分数带符号：正数加 "+"，用于信用变动列
 * @param {number} v
 */
export function signText(v) {
  const n = Number(v)
  if (!Number.isFinite(n)) return '—'
  return n > 0 ? `+${n}` : String(n)
}

/**
 * 手机号打码（演示环境用，避免泄露）
 * @param {string} phone
 */
export function maskPhone(phone) {
  const p = String(phone || '')
  if (p.length < 7) return p || '—'
  return `${p.slice(0, 3)}****${p.slice(-4)}`
}

/**
 * 信用分颜色（0–100）：<60 红、<80 黄、其余绿
 * @param {number} score
 */
export function creditColor(score) {
  const n = Number(score)
  if (!Number.isFinite(n)) return '#909399'
  if (n < 60) return '#f56c6c'
  if (n < 80) return '#e6a23c'
  return '#67c23a'
}

/**
 * 取日期部分 "2026-11-22"
 * @param {string} iso
 */
export function dateOf(iso) {
  return iso ? String(iso).slice(0, 10) : ''
}

/**
 * 本地今天的 "yyyy-MM-dd"（只给「查占用」默认日期用，不参与业务判断）
 */
export function today() {
  const d = new Date()
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`
}

/**
 * 今天 +n 天的 "yyyy-MM-dd"
 * @param {number} days
 */
export function dayAfter(days) {
  const d = new Date(Date.now() + days * 86400000)
  const p = (n) => String(n).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`
}
