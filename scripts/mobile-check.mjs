#!/usr/bin/env node
// =============================================================================
// scripts/mobile-check.mjs —— 移动端布局自检（零依赖，CDP over WebSocket）
//
// 为什么不用 --window-size=375,812：实测那给不出 375 视口（clientWidth=492），
// 任何基于它的断言都是假的。正确做法是 CDP 的 Emulation.setDeviceMetricsOverride。
//
// 做的事：
//   1) 起一个 headless Edge（--remote-debugging-port=9333）
//   2) 通过 CDP 设置 375×812 / dpr 2 / mobile
//   3) 注入登录态（真后端 token，写 H5 的 storage key）
//   4) 逐路由导航 → 量 clientWidth / scrollWidth / 溢出元素 → 截图
//   5) 输出表格；任意一条 hScroll 即判不通过（exit 1）
//
// 用法：
//   node scripts/mobile-check.mjs                 # 默认量 http://127.0.0.1:5174（dev）
//   node scripts/mobile-check.mjs --base http://127.0.0.1:8080/h5
// =============================================================================
import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

// ---------------------------------------------------------------- 参数
const argv = process.argv.slice(2)
const arg = (name, def) => {
  const i = argv.indexOf(`--${name}`)
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def
}

const BASE = arg('base', 'http://127.0.0.1:5174').replace(/\/$/, '')
const API = arg('api', 'http://127.0.0.1:8080')
const USER = arg('user', 'student01')
const PASS = arg('pass', 'User@123456')
const PORT = Number(arg('port', '9333'))
const SHOT_DIR = path.join(__dirname, 'shots')

const EDGE_CANDIDATES = [
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
]

/**
 * 8 条路由（与 router/index.js 一致）
 * anonymous: true 的路由会先清掉 token —— 用来验证「未登录也能进」的公开页
 * （P6 新增 /register 就在这一类：注册页必须匿名可访问，且 375 视口下不溢出）
 */
const ROUTES = [
  { path: '/home', name: 'home' },
  { path: '/resource/1', name: 'resource-detail' },
  { path: '/reservations', name: 'reservations' },
  { path: '/waitlist', name: 'waitlist' },
  { path: '/credit', name: 'credit' },
  { path: '/profile', name: 'profile' },
  { path: '/register', name: 'register', anonymous: true }, // P6
  { path: '/login', name: 'login', anonymous: true },
]

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

// ---------------------------------------------------------------- CDP 客户端
class Cdp {
  constructor(ws) {
    this.ws = ws
    this.id = 0
    this.pending = new Map()
    ws.addEventListener('message', (ev) => {
      let msg
      try {
        msg = JSON.parse(ev.data)
      } catch {
        return
      }
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id)
        this.pending.delete(msg.id)
        if (msg.error) reject(new Error(`${msg.error.message} (${msg.error.code})`))
        else resolve(msg.result)
      }
    })
  }

  send(method, params = {}, sessionId) {
    this.id += 1
    const id = this.id
    const payload = { id, method, params }
    if (sessionId) payload.sessionId = sessionId
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error(`CDP 超时：${method}`))
      }, 20000)
      this.pending.set(id, {
        resolve: (v) => {
          clearTimeout(timer)
          resolve(v)
        },
        reject: (e) => {
          clearTimeout(timer)
          reject(e)
        },
      })
      this.ws.send(JSON.stringify(payload))
    })
  }
}

async function waitForEndpoint(port, timeoutMs = 20000) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`http://127.0.0.1:${port}/json/version`)
      if (res.ok) {
        const j = await res.json()
        if (j.webSocketDebuggerUrl) return j.webSocketDebuggerUrl
      }
    } catch {
      /* 还没起来 */
    }
    await sleep(300)
  }
  throw new Error(`浏览器调试端口 ${port} 在 ${timeoutMs}ms 内未就绪`)
}

// ---------------------------------------------------------------- 主流程
async function main() {
  // ① 拿真实 token（直接打后端，不依赖前端代理）
  const loginRes = await fetch(`${API}/api/v1/auth/login`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: USER, password: PASS }),
  })
  const loginJson = await loginRes.json()
  if (loginJson.code !== 0) throw new Error(`登录失败：${loginJson.msg}`)
  const { token, user } = loginJson.data
  console.log(`登录成功：${user.username}（${user.role}）  度量目标：${BASE}`)

  // ② 起 headless Edge
  const edge = EDGE_CANDIDATES.find((p) => existsSync(p))
  if (!edge) throw new Error('未找到 msedge.exe / chrome.exe')
  const profile = path.join(process.env.TEMP || '.', `rf-mobile-check-${Date.now()}`)
  const child = spawn(
    edge,
    [
      '--headless=new',
      '--disable-gpu',
      '--no-sandbox',
      '--hide-scrollbars',
      `--remote-debugging-port=${PORT}`,
      `--user-data-dir=${profile}`,
      'about:blank',
    ],
    { stdio: 'ignore' }
  )

  const cleanup = () => {
    try {
      child.kill()
    } catch {
      /* ignore */
    }
  }
  process.on('exit', cleanup)

  const wsUrl = await waitForEndpoint(PORT)
  const ws = new WebSocket(wsUrl)
  await new Promise((resolve, reject) => {
    ws.addEventListener('open', resolve, { once: true })
    ws.addEventListener('error', () => reject(new Error('WebSocket 连接失败')), { once: true })
  })
  const cdp = new Cdp(ws)

  // ③ 建目标 + attach（flat 模式，后续命令都带 sessionId）
  const { targetId } = await cdp.send('Target.createTarget', { url: 'about:blank' })
  const { sessionId } = await cdp.send('Target.attachToTarget', { targetId, flatten: true })

  await cdp.send('Page.enable', {}, sessionId)
  await cdp.send('Runtime.enable', {}, sessionId)

  /** 375×812 / dpr 2 / mobile —— 这是唯一能真正得到 375 视口的方式 */
  await cdp.send(
    'Emulation.setDeviceMetricsOverride',
    { width: 375, height: 812, deviceScaleFactor: 2, mobile: true },
    sessionId
  )

  const evaluate = async (expression) => {
    const r = await cdp.send(
      'Runtime.evaluate',
      { expression, returnByValue: true, awaitPromise: true },
      sessionId
    )
    if (r.exceptionDetails) throw new Error(r.exceptionDetails.text || 'evaluate 失败')
    return r.result?.value
  }

  const navigate = async (url) => {
    await cdp.send('Page.navigate', { url }, sessionId)
    // 等路由渲染 + 接口返回（本地后端 <50ms，1.4s 足够）
    await sleep(1400)
  }

  // ④ 先把登录态写进该 origin 的 localStorage
  await navigate(`${BASE}/`)
  await evaluate(
    `localStorage.setItem('rf_h5_token', ${JSON.stringify(token)});
     localStorage.setItem('rf_h5_user', ${JSON.stringify(JSON.stringify(user))});
     'ok'`
  )

  // ⑤ 逐路由测量
  if (!existsSync(SHOT_DIR)) mkdirSync(SHOT_DIR, { recursive: true })

  /** 量：视口宽 / 文档宽 / 溢出元素清单 / 首屏文案 */
  // 判定口径（重要）：
  //   「溢出」= 元素右边界超出视口，**且它没有任何可横向滚动的祖先**。
  //   日期条、Vant 的 tab 导航这类容器本身就是 overflow-x:auto 的设计（滑动查看更多），
  //   它们的孩子超出视口是正常的；把它们算成溢出会产生大量假阳性。
  const MEASURE = `(() => {
    const vw = window.innerWidth;
    const de = document.documentElement;

    const scrollableAncestor = (el) => {
      let p = el.parentElement;
      while (p && p !== document.body) {
        const cs = getComputedStyle(p);
        if (/(auto|scroll)/.test(cs.overflowX)) return p;
        p = p.parentElement;
      }
      return null;
    };

    const overflow = [];
    document.querySelectorAll('body *').forEach((el) => {
      const r = el.getBoundingClientRect();
      if (r.width === 0 || r.height === 0) return;
      if (r.right <= vw + 1) return;
      if (scrollableAncestor(el)) return; // 故意横向滑动的容器，跳过
      overflow.push({
        tag: el.tagName.toLowerCase(),
        cls: (el.className && String(el.className).slice(0, 40)) || '',
        right: Math.round(r.right),
      });
    });

    return {
      innerWidth: vw,
      clientWidth: de.clientWidth,
      scrollWidth: Math.max(de.scrollWidth, document.body.scrollWidth),
      overflow: overflow.slice(0, 6),
      overflowCount: overflow.length,
      text: (document.body.innerText || '').replace(/\\s+/g, ' ').slice(0, 80),
    };
  })()`

  const rows = []
  for (const route of ROUTES) {
    // /login 要匿名：先清 token，量完再写回
    if (route.anonymous) {
      await evaluate(`localStorage.removeItem('rf_h5_token'); 'cleared'`)
    }

    // 关键：每次都用**不同的 query** 导航，强制整页重载 ——
    // 否则 hash 变化只是同文档路由，Pinia 里的 token 还是首次启动时的空值（会被守卫弹回 /login）。
    await navigate(`${BASE}/?r=${Date.now()}#${route.path}`)

    const m = await evaluate(MEASURE)

    // 截图
    const shot = await cdp.send('Page.captureScreenshot', { format: 'png' }, sessionId)
    writeFileSync(path.join(SHOT_DIR, `${route.name}.png`), Buffer.from(shot.data, 'base64'))

    rows.push({
      route: route.path,
      name: route.name,
      clientWidth: m.clientWidth,
      scrollWidth: m.scrollWidth,
      hScroll: m.scrollWidth > m.clientWidth,
      overflowCount: m.overflowCount,
      overflow: m.overflow,
      text: m.text,
    })

    if (route.anonymous) {
      await evaluate(
        `localStorage.setItem('rf_h5_token', ${JSON.stringify(token)});
         localStorage.setItem('rf_h5_user', ${JSON.stringify(JSON.stringify(user))});
         'restored'`
      )
    }
  }

  // ⑥ 输出表格
  console.log('')
  const head = ['路由', 'clientWidth', 'scrollWidth', '横向滚动', '溢出元素']
  const widths = [22, 12, 12, 10, 10]
  const line = (cells) =>
    cells.map((c, i) => String(c).padEnd(widths[i] ?? 12)).join('  ')
  console.log(line(head))
  console.log(line(widths.map((w) => '-'.repeat(w))))
  for (const r of rows) {
    console.log(
      line([
        r.route,
        r.clientWidth,
        r.scrollWidth,
        r.hScroll ? 'YES ⚠️' : 'no',
        r.overflowCount,
      ])
    )
  }

  const bad = rows.filter((r) => r.hScroll || r.overflowCount > 0 || r.clientWidth !== 375)
  console.log('')
  for (const r of rows) {
    console.log(`  ${r.route}  首屏：${r.text.slice(0, 70) || '(空)'}`)
  }

  if (bad.length) {
    console.log('\n有问题的路由：')
    for (const r of bad) {
      console.log(
        `  ${r.route}: clientWidth=${r.clientWidth} scrollWidth=${r.scrollWidth} 溢出=${JSON.stringify(r.overflow)}`
      )
    }
  }

  console.log(`\n截图已存：${SHOT_DIR}`)
  console.log(
    bad.length === 0
      ? `\n===== 结论：${ROUTES.length} 条路由全部 clientWidth=375 且无横向滚动 =====`
      : `\n===== 结论：${bad.length} 条路由不通过 =====`
  )

  ws.close()
  cleanup()

  return bad.length === 0 ? 0 : 1
}

main()
  .then((code) => process.exit(code))
  .catch((e) => {
    console.error(`\n❌ mobile-check 失败：${e.message}`)
    process.exit(2)
  })
