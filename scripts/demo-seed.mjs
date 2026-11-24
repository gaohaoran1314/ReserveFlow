#!/usr/bin/env node
// =============================================================================
// scripts/demo-seed.mjs —— 造演示数据（零依赖，只用 Node 18+ 内置 fetch）
//
// 目的：让管理后台 6 个页面都有内容可看，而不是打开一片空白。
// 产物（幂等性靠"编号/时间窗"识别，重复执行只会命中已存在的数据）：
//   · 1 个会议室资源（若已存在同类资源则复用）
//   · 1 条 PENDING_REVIEW（实验室仪器，走审核流）→ 让「待审核」卡片与审核按钮有内容
//   · 1 条 LOCKED（会议室，已审核）→ 让「取用」按钮可点
//   · 1 条 COMPLETED（会议室，走完 pickup→return→accept）→ 让押金流水/时间线有内容
//   · 1 条候补（挂在上面那条 LOCKED 的时段上，排队第 1 位）
//
// 用法：
//   node scripts/demo-seed.mjs                 # 默认 http://127.0.0.1:8080
//   node scripts/demo-seed.mjs --base http://127.0.0.1:8080 --user admin --pass Admin@123456
// =============================================================================

const args = process.argv.slice(2)
const arg = (name, def) => {
  const i = args.indexOf(`--${name}`)
  return i >= 0 && args[i + 1] ? args[i + 1] : def
}

const BASE = arg('base', 'http://127.0.0.1:8080')
const USER = arg('user', 'admin')
const PASS = arg('pass', 'Admin@123456')
const API = `${BASE}/api/v1`

let TOKEN = ''

/** 统一请求：把后端 {code,msg,data} 解一层，非 0 抛错 */
async function call(method, path, body) {
  const res = await fetch(`${API}${path}`, {
    method,
    headers: {
      'Content-Type': 'application/json',
      ...(TOKEN ? { Authorization: `Bearer ${TOKEN}` } : {}),
    },
    body: body === undefined ? undefined : JSON.stringify(body),
  })
  const text = await res.text()
  let json
  try {
    json = JSON.parse(text)
  } catch {
    throw new Error(`${method} ${path} 返回非 JSON（HTTP ${res.status}）：${text.slice(0, 120)}`)
  }
  if (json.code !== 0) {
    const err = new Error(`[${json.code}] ${json.msg}`)
    err.code = json.code
    err.path = path
    throw err
  }
  return json.data
}

/** 试探性调用：容忍特定错误码（用于幂等） */
async function tryCall(method, path, body, ignoreCodes = []) {
  try {
    return await call(method, path, body)
  } catch (e) {
    if (ignoreCodes.includes(e.code)) return null
    throw e
  }
}

/** 未来第 n 天的 "yyyy-MM-ddTHH:mm:ss"（只算日期，不做时区转换） */
function dayAt(n, hhmm) {
  const d = new Date(Date.now() + n * 86400000)
  const p = (x) => String(x).padStart(2, '0')
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${hhmm}:00`
}

function head(t) {
  console.log(`\n=== ${t} ===`)
}

async function main() {
  console.log(`ReserveFlow 演示数据种子`)
  console.log(`后端：${BASE}   账号：${USER}`)

  // ---------------------------------------------------------------- 登录
  head('1. 登录')
  const login = await call('POST', '/auth/login', { username: USER, password: PASS })
  TOKEN = login.token
  console.log(`  登录成功：${login.user.username}（${login.user.role}，信用 ${login.user.creditScore}）`)

  // ---------------------------------------------------------------- 资源
  head('2. 准备资源')
  const resources = await call('GET', '/resources?page=1&size=100')
  const list = resources.list || []
  const lab = list.find((r) => r.type === 'LAB_EQUIPMENT' && r.status === 'FREE')
  const meeting =
    list.find((r) => r.type === 'MEETING_ROOM' && r.status === 'FREE') ||
    list.find((r) => r.type === 'MEETING_ROOM')

  if (!lab) {
    console.log('  ⚠️ 没有可约的实验室仪器，跳过"待审核"示例（其余照常）')
  } else {
    console.log(`  实验室仪器：${lab.code} ${lab.name}（押金 ${lab.depositAmount}）`)
  }
  if (!meeting) {
    throw new Error('没有会议室资源，无法继续。请先用后台「资源管理」新建一个会议室')
  }
  console.log(`  会议室：${meeting.code} ${meeting.name}（押金 ${meeting.depositAmount}）`)

  // ---------------------------------------------------------------- 待审核
  head('3. 造一条待审核预约（实验室仪器 → 需审核）')
  let pending = null
  if (lab) {
    const span = { startTime: dayAt(3, '09:00'), endTime: dayAt(3, '10:00') }
    pending = await tryCall(
      'POST',
      '/reservations',
      { resourceId: lab.id, ...span, remark: '演示数据：待审核示例' },
      [3005]
    )
    if (pending) {
      console.log(`  ✅ ${pending.reservationNo}  status=${pending.status}  id=${pending.id}`)
      console.log(`     后台地址：${BASE}/admin/#/reservations?id=${pending.id}`)
    } else {
      console.log('  ⚠️ 该时段已被占用，跳过')
    }
  }

  // ---------------------------------------------------------------- 已锁定
  head('4. 造一条已锁定预约（会议室 → 免审核，直接 LOCKED）')
  const lockedSpan = { startTime: dayAt(4, '14:00'), endTime: dayAt(4, '15:00') }
  const locked = await tryCall(
    'POST',
    '/reservations',
    { resourceId: meeting.id, ...lockedSpan, remark: '演示数据：已锁定示例' },
    [3005]
  )
  if (locked) {
    console.log(`  ✅ ${locked.reservationNo}  status=${locked.status}  id=${locked.id}`)
    console.log(`     后台地址：${BASE}/admin/#/reservations?id=${locked.id}`)
  } else {
    console.log('  ⚠️ 该时段已被占用，跳过')
  }

  // ---------------------------------------------------------------- 候补
  head('5. 造一条候补（挂在上面那条已锁定时段上）')
  if (locked) {
    const w = await tryCall(
      'POST',
      `/resources/${meeting.id}/waitlist`,
      { startTime: lockedSpan.startTime, endTime: lockedSpan.endTime },
      [3020]
    )
    if (w) {
      console.log(`  ✅ 候补 id=${w.id} status=${w.status} position=${w.position}`)
    } else {
      console.log('  ⚠️ 该时段已在候补队列中，跳过')
    }
  }

  // ---------------------------------------------------------------- 已完成（走完整闭环）
  head('6. 造一条已完成预约（下单 → 取用 → 归还 → 验收）')

  // 取用窗口 = [start-15min, start+30min]，而时段必须整点/半点对齐。
  // 所以只有「下一个半点距现在 <= 14 分钟」时才能立刻取用；否则如实说明并保留 LOCKED。
  const now = new Date()
  const nextBoundary = new Date(now)
  nextBoundary.setSeconds(0, 0)
  nextBoundary.setMinutes(now.getMinutes() < 30 ? 30 : 60)
  const minutesToBoundary = Math.round((nextBoundary - now) / 60000)

  const p2 = (x) => String(x).padStart(2, '0')
  const startIso =
    `${nextBoundary.getFullYear()}-${p2(nextBoundary.getMonth() + 1)}-${p2(nextBoundary.getDate())}` +
    `T${p2(nextBoundary.getHours())}:${p2(nextBoundary.getMinutes())}:00`
  const endBoundary = new Date(nextBoundary.getTime() + 30 * 60000)
  const endIso =
    `${endBoundary.getFullYear()}-${p2(endBoundary.getMonth() + 1)}-${p2(endBoundary.getDate())}` +
    `T${p2(endBoundary.getHours())}:${p2(endBoundary.getMinutes())}:00`

  const done = await tryCall(
    'POST',
    '/reservations',
    { resourceId: meeting.id, startTime: startIso, endTime: endIso, remark: '演示数据：闭环示例' },
    [3005, 3009]
  )

  if (!done) {
    console.log('  ⚠️ 该时段已被占用，跳过')
  } else if (minutesToBoundary > 14) {
    console.log(`  ⚠️ 距下一个半点还有 ${minutesToBoundary} 分钟，取用窗口（开始前 15 分钟）尚未开放。`)
    console.log(`     该单已保留为 ${done.status}：${done.reservationNo} id=${done.id}`)
    console.log(`     「已完成」的时间线与押金流水请用历史单：${BASE}/admin/#/reservations?id=239`)
    console.log(`     想自动造出「已完成」示例：约 ${minutesToBoundary - 14} 分钟后再跑一次 npm run seed`)
  } else {
    const id = done.id
    try {
      await call('POST', `/reservations/${id}/pickup`, {})
      await call('POST', `/reservations/${id}/return`, { remark: '演示数据：提前归还' })
      const accepted = await call('POST', `/reservations/${id}/accept`, { remark: '演示数据：验收通过' })
      console.log(`  ✅ ${done.reservationNo}  status=${accepted.status}  id=${id}`)
      console.log(`     押金：${JSON.stringify(accepted.deposit || {})}`)
      console.log(`     后台地址：${BASE}/admin/#/reservations?id=${id}`)
    } catch (e) {
      console.log(`  ⚠️ 闭环未走完（${e.message}），该单保持 ${done.status}`)
    }
  }

  // ---------------------------------------------------------------- 汇总
  head('7. 完成')
  const resv = await call('GET', '/reservations?page=1&size=100')
  const byStatus = {}
  for (const r of resv.list || []) byStatus[r.status] = (byStatus[r.status] || 0) + 1
  console.log(`  当前共 ${resv.total} 条预约：${JSON.stringify(byStatus)}`)

  const users = await call('GET', '/admin/users?page=1&size=100')
  console.log(`  账号 ${users.total} 个：${(users.list || []).map((u) => `${u.username}(${u.creditScore}/${u.status})`).join('  ')}`)

  console.log('\n  管理后台：')
  console.log(`    ${BASE}/admin/            （或 npm run dev → http://localhost:5173）`)
  console.log(`    账号：${USER} / ${PASS}`)
  console.log('    页面：/dashboard  /resources  /reservations  /rules  /users  /system')
}

main().catch((e) => {
  console.error(`\n❌ 种子执行失败：${e.message}`)
  if (e.path) console.error(`   接口：${e.path}`)
  console.error('   请确认后端已启动、MySQL/Redis 正常。')
  process.exit(1)
})
