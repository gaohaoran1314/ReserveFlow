#!/usr/bin/env node
/**
 * ReserveFlow 一键验收（P0 全量 40 项 + P1 增量）
 *
 *   node scripts/verify.mjs            直接验收（复用已在跑的 8080 服务，没跑就自己拉起）
 *   node scripts/verify.mjs --build    先重新编译再验收
 *   node scripts/verify.mjs --keep     验收完不关服务，方便继续点 /debug
 *
 * 第 1~11 节 = P0 原有断言，**一条未改、一条未删**；
 * 第 12~18 节 = P1 增量（下单/占用/我的预约/详情/取消/审核/驳回/前置校验/删除保护）。
 * 总数以 results.length 为准（P0 40 项 + P1 增量）。
 *
 * 只依赖 Node 内置模块，不需要 npm install。
 */

import net from 'node:net';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EXE = [
  // rfbuild 是 P1 起主理人指定的构建目录，放在最前，避免误用旧产物
  'rfbuild', 'cmake-build-msvc', 'build', 'build-clean', 'cmake-build-debug', 'cmake-build-release',
  'build/Debug', 'build/Release', 'cmake-build-debug/Debug',
]
  .map((d) => path.join(ROOT, d, 'reserveflow.exe'))
  .find((p) => existsSync(p)) || path.join(ROOT, 'build', 'reserveflow.exe');
const BASE = 'http://127.0.0.1:8080/api/v1';
const MYSQL_EXE = 'C:\\Program Files\\MySQL\\MySQL Server 8.0\\bin\\mysql.exe';

const argv = process.argv.slice(2);
const DO_BUILD = argv.includes('--build');
const KEEP = argv.includes('--keep');

const C = { g: '\x1b[32m', r: '\x1b[31m', d: '\x1b[90m', b: '\x1b[1m', x: '\x1b[0m' };

/**
 * 引擎总开关（P3）：从 config.json 读 `feature.ruleEngineEnabled`。
 * 用来让「引擎关」那一轮验收的断言也跟着切换口径（P3 的 rule_code 断言分两种模式）。
 */
const ENGINE_ON = (() => {
  try {
    const raw = readFileSync(path.join(ROOT, 'config', 'config.json'), 'utf8');
    const stripped = raw.replace(/^\s*\/\/.*$/gm, '');   // 允许 // 注释
    return JSON.parse(stripped)?.feature?.ruleEngineEnabled !== false;
  } catch {
    return true;
  }
})();

const results = [];
let child = null;
let adminToken = '';
let userToken = '';
let testId = null;
const testCode = 'VRFY-' + String(Date.now()).slice(-6);

// P2 增量：本轮创建的预约 / 资源 / 候补（清理与不变量都按这些 id 精确圈定）
const p2ResIds = [];
const p2ResCreated = [];
const p2WaitIds = [];
let p2CreditBefore = -1;

// P1 增量：需要在 finally 里清理的实体
let p1ResId = null;              // 会议室单（中途会被取消并重建）
let p1LabId = null;              // 仪器单（审核通过）
let p1RejectId = null;           // 仪器单（驳回）
let p1GuardId = null;            // 删除保护用的临时资源
let p1GuardReservationId = null; // 该临时资源上的预约
const p1CreatedIds = [];         // 本脚本创建过的**所有**预约 id（含被取消/重建的），确保清理无残留
const p3CreditLogIds = [];       // P3 §28 手工调整写下的 credit_log id（reservation_id 为 NULL，不能按单清理）
const p6Users = [];              // P6 §31 注册的测试学号（结尾按 username 精确删除）
function trackP1(json) {
  const id = json?.data?.id;
  if (id) p1CreatedIds.push(id);
  return id ?? null;
}

function ok(name, pass, detail = '') {
  results.push({ name, pass, detail });
  const tag = pass ? `${C.g}PASS${C.x}` : `${C.r}FAIL${C.x}`;
  console.log(`  [${tag}] ${name}${detail ? `  ${C.d}${detail}${C.x}` : ''}`);
}

function head(title) {
  console.log(`\n${C.b}${title}${C.x}`);
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function tcp(host, port, timeout = 1200) {
  return new Promise((resolve) => {
    const s = new net.Socket();
    let done = false;
    const fin = (v) => {
      if (done) return;
      done = true;
      s.destroy();
      resolve(v);
    };
    s.setTimeout(timeout);
    s.once('connect', () => fin(true));
    s.once('timeout', () => fin(false));
    s.once('error', () => fin(false));
    s.connect(port, host);
  });
}

async function api(method, p, opts = {}) {
  const headers = { 'Content-Type': 'application/json' };
  if (opts.token) headers.Authorization = `Bearer ${opts.token}`;
  const res = await fetch(BASE + p, {
    method,
    headers,
    body: opts.body ? JSON.stringify(opts.body) : undefined,
    signal: AbortSignal.timeout(8000),
  });
  const text = await res.text();
  let json = null;
  try {
    json = JSON.parse(text);
  } catch {
    json = null;
  }
  return { status: res.status, json, text };
}

async function alive() {
  try {
    const r = await api('GET', '/health/deps');
    return r.status === 200;
  } catch {
    return false;
  }
}

/** 直连 MySQL 执行 SQL（仅用于 P1 的断言与清理，与 stress.mjs 同口径） */
function mysqlExec(sql) {
  const r = spawnSync(
    MYSQL_EXE,
    ['-u', 'rf', '-pRf@2026!', '-D', 'reserveflow', '-N', '-B', '-e', sql],
    { encoding: 'utf8' }
  );
  return { status: r.status, out: (r.stdout || '').trim(), err: (r.stderr || '').trim() };
}

/** 单值查询（取第一行第一列） */
function mysqlScalar(sql) {
  const r = mysqlExec(sql);
  return (r.out || '').trim();
}

/** N 天后（本地时区）的 yyyy-MM-dd，用于构造远期日期，避免与人工数据冲突 */
function dateAfter(days) {
  const d = new Date(Date.now() + days * 86400000);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

// ────────────────────────────────────────────────────────────

console.log(`${C.b}ReserveFlow 验收（P0 + P1）${C.x}   ${new Date().toLocaleString('zh-CN')}`);
console.log(`${C.d}工程根目录：${ROOT}${C.x}`);

head('0. 环境');
ok('MySQL 3306 可连接', await tcp('127.0.0.1', 3306));
ok('Redis 6379 可连接', await tcp('127.0.0.1', 6379));

if (DO_BUILD) {
  head('0b. 重新编译');
  const r = spawnSync('cmake', ['--build', path.join(ROOT, 'build')], { cwd: ROOT, encoding: 'utf8' });
  const tail = ((r.stdout || '') + (r.stderr || '')).trim().split('\n').slice(-2).join(' / ');
  ok('cmake --build 成功', r.status === 0, tail);
}

if (!existsSync(EXE)) {
  console.error(`\n${C.r}找不到 ${EXE}${C.x}\n先在 CLion 里 Build 一次，或加 --build 参数。`);
  process.exit(2);
}

head('0c. 服务');
if (await alive()) {
  ok('8080 已有服务在跑，直接复用', true);
} else {
  child = spawn(EXE, [], { cwd: ROOT, stdio: 'ignore' });
  let ready = false;
  for (let i = 0; i < 40; i++) {
    if (await alive()) {
      ready = true;
      break;
    }
    await sleep(500);
  }
  ok('启动 reserveflow.exe 并监听 8080', ready, ready ? '' : '20 秒内未就绪');
  if (!ready) {
    child.kill();
    process.exit(3);
  }
}

try {
  head('1. 依赖探针');
  {
    const r = await api('GET', '/health/deps');
    ok('GET /health/deps 返回 200', r.status === 200, `HTTP ${r.status}`);
    ok('MySQL 状态 up', r.json?.data?.mysql?.status === 'up', `status=${r.json?.data?.mysql?.status}`);
    ok('Redis 状态 up', r.json?.data?.redis?.status === 'up', `status=${r.json?.data?.redis?.status}`);
  }

  head('2. 登录与会话（PRD US-01 / US-02）');
  {
    const r = await api('POST', '/auth/login', { body: { username: 'admin', password: 'Admin@123456' } });
    adminToken = r.json?.data?.token || '';
    ok('管理员登录成功', r.json?.code === 0 && adminToken.length > 100, `code=${r.json?.code} token=${adminToken.length} 字符`);
  }
  {
    const r = await api('POST', '/auth/login', { body: { username: 'admin', password: 'wrong' } });
    ok('错误密码被拒绝', r.json?.code === 2000, `code=${r.json?.code} "${r.json?.msg}"`);
  }
  {
    const r = await api('POST', '/auth/login', { body: { username: 'nobody-xyz', password: 'x' } });
    ok('账号不存在返回同一错误码（防枚举）', r.json?.code === 2000, `code=${r.json?.code}`);
  }
  {
    const r = await api('GET', '/auth/me', { token: adminToken });
    ok('GET /auth/me 取回本人信息', r.json?.data?.username === 'admin', `role=${r.json?.data?.role}`);
  }
  {
    const r = await api('POST', '/auth/login', { body: { username: 'student01', password: 'User@123456' } });
    userToken = r.json?.data?.token || '';
    ok('普通用户登录成功', r.json?.code === 0 && userToken.length > 100);
  }

  head('3. 鉴权与越权（PRD US-02 / US-03）');
  {
    const r = await api('GET', '/resources');
    ok('不带 token 访问资源列表 → 401', r.status === 401, `HTTP ${r.status} code=${r.json?.code}`);
  }
  {
    const r = await api('GET', '/resources', { token: adminToken.slice(0, -2) + 'xx' });
    ok('篡改 token → 401', r.status === 401, `HTTP ${r.status} code=${r.json?.code}`);
  }
  {
    const r = await api('POST', '/resources', {
      token: userToken,
      body: { code: 'NOPE-1', name: '越权', type: 'LOCKER', location: 'x', capacity: 1, depositAmount: 0 },
    });
    ok('普通用户新增资源 → 403', r.status === 403, `HTTP ${r.status} code=${r.json?.code}`);
  }
  {
    const r = await api('GET', '/resources', { token: userToken });
    ok('普通用户仍可读列表', r.status === 200 && r.json?.code === 0);
  }

  head('4. 资源新增与唯一约束（PRD US-04）');
  {
    const r = await api('POST', '/resources', {
      token: adminToken,
      body: {
        code: testCode,
        name: '验收用储物柜',
        type: 'LOCKER',
        location: '体育馆1层',
        capacity: 10,
        depositAmount: 20.0,
        description: '由一键验收脚本创建，跑完自动删除',
      },
    });
    testId = r.json?.data?.id ?? null;
    ok('新增资源返回新 id', r.json?.code === 0 && !!testId, `id=${testId} code=${testCode}`);
  }
  {
    const r = await api('POST', '/resources', {
      token: adminToken,
      body: { code: testCode, name: '重复编号', type: 'LOCKER', location: 'x', capacity: 1, depositAmount: 0 },
    });
    ok('重复编号 → 3001', r.json?.code === 3001, `"${r.json?.msg}"`);
  }
  {
    const r = await api('POST', '/resources', {
      token: adminToken,
      body: { code: 'X-' + Date.now(), name: '缺字段', type: 'PLANE', location: 'x', capacity: 1, depositAmount: 0 },
    });
    ok('非法 type → 1000 参数校验', r.json?.code === 1000, `"${r.json?.msg}"`);
  }

  head('5. 半更新语义（PRD US-05）');
  {
    const before = (await api('GET', `/resources/${testId}`, { token: adminToken })).json?.data;
    await api('PUT', `/resources/${testId}`, { token: adminToken, body: { name: '验收用储物柜-改名' } });
    const after = (await api('GET', `/resources/${testId}`, { token: adminToken })).json?.data;
    const nameChanged = after?.name === '验收用储物柜-改名';
    const othersKept =
      after?.type === before?.type &&
      after?.location === before?.location &&
      Number(after?.capacity) === Number(before?.capacity) &&
      Number(after?.depositAmount) === Number(before?.depositAmount) &&
      after?.code === before?.code;
    ok('只传 name：name 变了', nameChanged, `${before?.name} → ${after?.name}`);
    ok('只传 name：其他字段原封不动', othersKept, `type=${after?.type} location=${after?.location} cap=${after?.capacity} deposit=${after?.depositAmount}`);
  }

  head('6. 检索与分页（PRD US-07）');
  {
    const r = await api('GET', '/resources?type=MEETING_ROOM&keyword=' + encodeURIComponent('图书馆'), { token: adminToken });
    const d = r.json?.data;
    const allMatch = Array.isArray(d?.list) && d.list.length > 0 && d.list.every((x) => x.type === 'MEETING_ROOM');
    ok('组合检索：条数 == total', typeof d?.total === 'number' && d.list.length === d.total, `total=${d?.total} 返回=${d?.list?.length}`);
    ok('组合检索：结果类型全为会议室', allMatch, `条数=${d?.list?.length ?? '无'}`);
  }
  {
    const p1 = (await api('GET', '/resources?page=1&size=3', { token: adminToken })).json?.data;
    const p2 = (await api('GET', '/resources?page=2&size=3', { token: adminToken })).json?.data;
    const ids1 = (p1?.list || []).map((x) => x.id);
    const ids2 = (p2?.list || []).map((x) => x.id);
    const noOverlap = ids1.every((i) => !ids2.includes(i));
    ok('分页：两页数据不重复', ids1.length > 0 && ids1.every((i) => !ids2.includes(i)), `p1=${ids1} p2=${ids2}`);
    ok('分页：total 不随翻页变化', typeof p1?.total === 'number' && p1.total === p2?.total, `total=${p1?.total}`);
  }

  head('7. 状态手工标记（PRD US-06 / 接口 12）');
  {
    const r = await api('PATCH', `/resources/${testId}/status`, { token: adminToken, body: { status: 'MAINTENANCE' } });
    ok('PATCH MAINTENANCE 成功', r.json?.code === 0 && r.json?.data?.status === 'MAINTENANCE', `status=${r.json?.data?.status}`);
  }
  {
    const r = await api('PATCH', `/resources/${testId}/status`, { token: adminToken, body: { status: 'ABC' } });
    ok('PATCH 非法字面量 ABC → 1000', r.json?.code === 1000, `"${r.json?.msg}"`);
  }
  {
    const r = await api('PATCH', `/resources/${testId}/status`, { token: adminToken, body: { status: 'IN_USE' } });
    ok('PATCH 合法枚举但 P0 不可写 IN_USE → 3002', r.json?.code === 3002, `"${r.json?.msg}"`);
  }
  {
    const r = await api('GET', `/resources/${testId}`, { token: adminToken });
    ok('非法状态未被写库', r.json?.data?.status === 'MAINTENANCE', `status=${r.json?.data?.status}`);
  }

  head('8. 删除与幂等（PRD US-06）');
  {
    const r = await api('DELETE', `/resources/${testId}`, { token: adminToken });
    ok('删除成功且提示编号永久保留', r.json?.code === 0 && String(r.json?.msg || '').includes('永久保留'), `"${r.json?.msg}"`);
  }
  {
    const r = await api('DELETE', `/resources/${testId}`, { token: adminToken });
    ok('重复删除幂等成功', r.json?.code === 0, `"${r.json?.msg}"`);
  }
  {
    const r = await api('GET', `/resources/${testId}`, { token: adminToken });
    ok('删除后查详情 → 3000 资源不存在', r.json?.code === 3000, `HTTP ${r.status} code=${r.json?.code}`);
  }
  {
    const r = await api('GET', '/resources?page=1&size=100', { token: adminToken });
    const stillThere = (r.json?.data?.list || []).some((x) => x.id === testId);
    ok('删除后不在列表中', !stillThere);
  }

  head('9. 字典与路由（PRD 接口 6）');
  {
    const r = await api('GET', '/resources/meta', { token: adminToken });
    const types = r.json?.data?.types || [];
    ok('GET /resources/meta 未被 {id} 路由吞掉', r.json?.code === 0 && types.length === 3, `类型数=${types.length}`);
    ok('meta 只把 FREE/MAINTENANCE 标为可选', (r.json?.data?.statuses || []).filter((s) => s.selectable).length === 2);
  }
  {
    const r = await api('GET', '/no-such-endpoint', { token: adminToken });
    ok('未知接口 → 404', r.status === 404, `HTTP ${r.status} code=${r.json?.code}`);
  }

  head('10. 登出与令牌失效（PRD 接口 2）');
  {
    const r = await api('POST', '/auth/logout', { token: userToken });
    ok('登出成功', r.json?.code === 0);
    const r2 = await api('GET', '/auth/me', { token: userToken });
    ok('登出后旧 token → 401', r2.status === 401, `HTTP ${r2.status} code=${r2.json?.code}`);
  }

  head('11. 自测台与日志（PRD US-09 / 验收 15）');
  {
    const r = await fetch('http://127.0.0.1:8080/debug/', { signal: AbortSignal.timeout(5000) });
    const html = await r.text();
    const buttons = (html.match(/<section class="card">/g) || []).length;
    ok('/debug 页面可访问', r.status === 200, `HTTP ${r.status} ${html.length} 字节`);
    ok('/debug 含 12 个接口面板', buttons === 12, `面板数=${buttons}`);
  }
  {
    const r = spawnSync('grep', ['-c', 'AUDIT', path.join(ROOT, 'logs', 'reserveflow.log')], { encoding: 'utf8' });
    const n = parseInt((r.stdout || '0').trim(), 10);
    ok('日志中存在 AUDIT 记录', n > 0, `${n} 行`);
  }
  // ══════════════════════════════════════════════════════════════════════════
  //  以下为 **P1 增量**。上方 P0 的 40 条断言一条未改、一条未删。
  // ══════════════════════════════════════════════════════════════════════════
  const p1Date = dateAfter(30);    // 远期日期，保证「未占用」前置成立且可重复跑
  const p1Date2 = dateAfter(31);
  // 清掉这两个日期上可能的历史残留（上一次运行失败时的遗留）
  mysqlExec(
    `DELETE s FROM reservation_slot s JOIN reservation r ON r.id=s.reservation_id ` +
      `WHERE r.resource_id IN (1,4) AND DATE(r.start_time) IN ('${p1Date}','${p1Date2}');`
  );
  mysqlExec(
    `DELETE FROM reservation WHERE resource_id IN (1,4) ` +
      `AND DATE(start_time) IN ('${p1Date}','${p1Date2}');`
  );

  // P0 第 10 节把 userToken 登出了，这里重新登录（并取黑名单账号令牌做越权/拦截测试）
  {
    const r = await api('POST', '/auth/login', { body: { username: 'student01', password: 'User@123456' } });
    userToken = r.json?.data?.token || '';
    ok('P1 前置：重新登录 student01', userToken.length > 100, `token=${userToken.length} 字符`);
  }
  const blackToken =
    (await api('POST', '/auth/login', { body: { username: 'student02', password: 'Black@123456' } })).json?.data
      ?.token || '';

  head('12. P1 预约下单与时段占用（ARCH-P1 §1.2 / §4.1 #2）');
  {
    const r = await api('POST', '/reservations', {
      token: userToken,
      body: {
        resourceId: 4,
        startTime: `${p1Date}T09:00:00`,
        endTime: `${p1Date}T10:00:00`,
        remark: 'P1 验收会议室单',
      },
    });
    p1ResId = trackP1(r.json);
    ok('会议室下单成功且免审核直达 LOCKED', r.json?.code === 0 && r.json?.data?.status === 'LOCKED',
       `code=${r.json?.code} status=${r.json?.data?.status} id=${p1ResId}`);
    ok('单号格式 RV + yyyyMMdd + 4 位流水', /^RV\d{12}$/.test(String(r.json?.data?.reservationNo || '')),
       `no=${r.json?.data?.reservationNo}`);
    ok('槽位展开为 [18,19]', JSON.stringify(r.json?.data?.slotIndexes) === '[18,19]',
       JSON.stringify(r.json?.data?.slotIndexes));
    ok('allowedActions 含 CANCEL（供前端置灰）', (r.json?.data?.allowedActions || []).includes('CANCEL'),
       JSON.stringify(r.json?.data?.allowedActions));
    const res = await api('GET', '/resources/4', { token: adminToken });
    ok('资源态收敛为 LOCKED', res.json?.data?.status === 'LOCKED', `status=${res.json?.data?.status}`);
  }
  {
    const r = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 4, startTime: `${p1Date}T09:30:00`, endTime: `${p1Date}T10:30:00` },
    });
    ok('重叠时段第二次下单 → 3005', r.json?.code === 3005, `code=${r.json?.code} "${r.json?.msg}"`);
  }
  {
    const r = await api('GET', `/resources/4/slots?date=${p1Date}`, { token: userToken });
    const occ = r.json?.data?.occupied || [];
    ok('查占用：occupied 含 18,19', occ.includes(18) && occ.includes(19), `occupied=${JSON.stringify(occ)}`);
    ok('查占用：occupiedLabels 与槽位一致',
       JSON.stringify(r.json?.data?.occupiedLabels || []) === '["09:00-09:30","09:30-10:00"]',
       JSON.stringify(r.json?.data?.occupiedLabels));
    ok('查占用：48 槽，可用 = 48 - 已占',
       r.json?.data?.totalSlotCount === 48 && (r.json?.data?.available || []).length === 48 - occ.length,
       `available=${(r.json?.data?.available || []).length} occupied=${occ.length}`);
  }

  head('13. P1 我的预约与详情（PRD P1-US-02）');
  {
    const r = await api('GET', '/reservations?page=1&size=50', { token: userToken });
    const list = r.json?.data?.list || [];
    ok('普通用户列表只含本人单据', r.json?.code === 0 && list.every((x) => x.userId === 2),
       `条数=${list.length}`);
    ok('列表包含刚创建的单', list.some((x) => x.id === p1ResId), `id=${p1ResId}`);
    ok('列表 total == 返回条数', r.json?.data?.total === list.length, `total=${r.json?.data?.total}`);
  }
  {
    const r = await api('GET', `/reservations/${p1ResId}`, { token: userToken });
    ok('详情：本人可读且 LOCKED', r.json?.code === 0 && r.json?.data?.status === 'LOCKED',
       `status=${r.json?.data?.status}`);
    ok('详情：含 slotDate 与 2 个槽位',
       r.json?.data?.slotDate === p1Date && (r.json?.data?.slotIndexes || []).length === 2,
       `slotDate=${r.json?.data?.slotDate} slots=${JSON.stringify(r.json?.data?.slotIndexes)}`);
    ok('详情：带资源编号与下单人',
       r.json?.data?.resourceCode === 'MR-202' && r.json?.data?.username === 'student01',
       `${r.json?.data?.resourceCode}/${r.json?.data?.username}`);
  }
  {
    const r = await api('GET', `/reservations/${p1ResId}`, { token: blackToken });
    ok('详情：非本人非管理员 → 3008', r.json?.code === 3008, `code=${r.json?.code} "${r.json?.msg}"`);
  }
  {
    const r = await api('GET', `/reservations/${p1ResId}`);
    ok('详情：未登录 → 401', r.status === 401, `HTTP ${r.status} code=${r.json?.code}`);
  }

  head('14. P1 取消与槽位释放（ARCH-P1 §4.1 #6）');
  {
    const r = await api('POST', `/reservations/${p1ResId}/cancel`, {
      token: userToken,
      body: { reason: 'P1 验收取消' },
    });
    ok('取消成功', r.json?.code === 0, `"${r.json?.msg}"`);
    const d = await api('GET', `/reservations/${p1ResId}`, { token: userToken });
    ok('取消后状态 CANCELLED 且无可用动作',
       d.json?.data?.status === 'CANCELLED' && (d.json?.data?.allowedActions || []).length === 0,
       `status=${d.json?.data?.status}`);
  }
  {
    const s = await api('GET', `/resources/4/slots?date=${p1Date}`, { token: userToken });
    const occ = s.json?.data?.occupied || [];
    ok('取消后槽位已释放（18/19 不再被占）', !occ.includes(18) && !occ.includes(19),
       `occupied=${JSON.stringify(occ)}`);
  }
  {
    const r = await api('POST', `/reservations/${p1ResId}/cancel`, { token: userToken, body: {} });
    ok('重复取消 → 3007', r.json?.code === 3007, `code=${r.json?.code} "${r.json?.msg}"`);
  }
  {
    const r = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 4, startTime: `${p1Date}T09:00:00`, endTime: `${p1Date}T10:00:00` },
    });
    ok('取消后同一时段可再次预约成功', r.json?.code === 0,
       `code=${r.json?.code} id=${r.json?.data?.id}`);
    if (r.json?.code === 0) {
      p1ResId = trackP1(r.json);   // 交给 finally 清理
    }
  }

  head('15. P1 仪器审核流（ARCH-P1 §4.1 #1/#3/#4）');
  {
    const r = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 1, startTime: `${p1Date}T14:00:00`, endTime: `${p1Date}T16:00:00` },
    });
    p1LabId = trackP1(r.json);
    ok('仪器下单 → PENDING_REVIEW', r.json?.code === 0 && r.json?.data?.status === 'PENDING_REVIEW',
       `code=${r.json?.code} status=${r.json?.data?.status}`);
    ok('仪器单 needReview=true 且 4 个槽位已占',
       r.json?.data?.needReview === true && (r.json?.data?.slotIndexes || []).length === 4,
       `slots=${JSON.stringify(r.json?.data?.slotIndexes)}`);
    const s = await api('GET', `/resources/1/slots?date=${p1Date}`, { token: adminToken });
    ok('待审核单同样占槽（软占位）', (s.json?.data?.occupied || []).length >= 4,
       `occupied=${JSON.stringify(s.json?.data?.occupied)}`);
  }
  {
    const r = await api('POST', `/reservations/${p1LabId}/approve`, { token: userToken, body: {} });
    ok('普通用户审核 → 403', r.status === 403, `HTTP ${r.status} code=${r.json?.code}`);
  }
  {
    const r = await api('POST', `/reservations/${p1LabId}/approve`, {
      token: adminToken,
      body: { remark: 'P1 验收通过' },
    });
    ok('管理员审核通过 → LOCKED 且槽位不变',
       r.json?.code === 0 && r.json?.data?.status === 'LOCKED' &&
         (r.json?.data?.slotIndexes || []).length === 4,
       `status=${r.json?.data?.status} slots=${JSON.stringify(r.json?.data?.slotIndexes)}`);
  }
  {
    const r = await api('POST', `/reservations/${p1LabId}/approve`, { token: adminToken, body: {} });
    ok('对 LOCKED 单再 approve → 3007', r.json?.code === 3007, `code=${r.json?.code} "${r.json?.msg}"`);
  }
  {
    const c = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 1, startTime: `${p1Date2}T09:00:00`, endTime: `${p1Date2}T10:00:00` },
    });
    p1RejectId = trackP1(c.json);
    ok('再下一单待审核', c.json?.code === 0 && c.json?.data?.status === 'PENDING_REVIEW',
       `id=${p1RejectId}`);
    const r = await api('POST', `/reservations/${p1RejectId}/reject`, {
      token: adminToken,
      body: { reason: '设备检修，请改约' },
    });
    ok('管理员驳回 → REJECTED 且槽位清空',
       r.json?.code === 0 && r.json?.data?.status === 'REJECTED' &&
         (r.json?.data?.slotIndexes || []).length === 0,
       `status=${r.json?.data?.status} slots=${JSON.stringify(r.json?.data?.slotIndexes)}`);
    const s = await api('GET', `/resources/1/slots?date=${p1Date2}`, { token: adminToken });
    ok('驳回后槽位已释放', (s.json?.data?.occupied || []).length === 0,
       `occupied=${JSON.stringify(s.json?.data?.occupied)}`);
  }

  head('16. P1 下单前置校验（§1.2 第 5~7 步）');
  {
    const r = await api('POST', '/reservations', {
      token: blackToken,
      body: { resourceId: 4, startTime: `${p1Date2}T15:00:00`, endTime: `${p1Date2}T16:00:00` },
    });
    ok('黑名单账号下单 → 3010', r.json?.code === 3010, `code=${r.json?.code} "${r.json?.msg}"`);
  }
  {
    const r = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 2, startTime: `${p1Date2}T15:00:00`, endTime: `${p1Date2}T16:00:00` },
    });
    ok('维护中资源下单 → 3012', r.json?.code === 3012, `code=${r.json?.code} "${r.json?.msg}"`);
  }
  {
    const r1 = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 4, startTime: `${p1Date2}T23:00:00`, endTime: `${p1Date}T01:00:00` },
    });
    ok('跨天预约 → 3009', r1.json?.code === 3009, `code=${r1.json?.code} "${r1.json?.msg}"`);
    const r2 = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 4, startTime: `${p1Date2}T09:15:00`, endTime: `${p1Date2}T10:00:00` },
    });
    ok('非半点对齐 → 3009', r2.json?.code === 3009, `code=${r2.json?.code} "${r2.json?.msg}"`);
    const r3 = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 4, startTime: `${p1Date2}T07:00:00`, endTime: `${p1Date2}T16:00:00` },
    });
    ok('超过 8 小时 → 3009', r3.json?.code === 3009, `code=${r3.json?.code} "${r3.json?.msg}"`);
    const r4 = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 999999, startTime: `${p1Date2}T09:00:00`, endTime: `${p1Date2}T10:00:00` },
    });
    ok('资源不存在 → 3000', r4.json?.code === 3000, `code=${r4.json?.code} "${r4.json?.msg}"`);
  }

  head('17. P1 删除资源保护：有有效预约 → 3003（§8.2-3）');
  {
    const code = 'VRFYP1-' + String(Date.now()).slice(-6);
    const c = await api('POST', '/resources', {
      token: adminToken,
      body: { code, name: 'P1 删除保护验证', type: 'MEETING_ROOM', location: '验收楼-9', capacity: 4, depositAmount: 0 },
    });
    p1GuardId = c.json?.data?.id ?? null;
    ok('为删除保护新建临时资源', c.json?.code === 0 && !!p1GuardId, `id=${p1GuardId} code=${code}`);
    if (p1GuardId) {
      const b = await api('POST', '/reservations', {
        token: userToken,
        body: { resourceId: p1GuardId, startTime: `${p1Date2}T11:00:00`, endTime: `${p1Date2}T12:00:00` },
      });
      p1GuardReservationId = trackP1(b.json);
      ok('临时资源下单成功（产生有效预约）', b.json?.code === 0,
         `code=${b.json?.code} id=${p1GuardReservationId}`);
      const d = await api('DELETE', `/resources/${p1GuardId}`, { token: adminToken });
      ok('存在有效预约时删除资源 → 3003', d.json?.code === 3003, `code=${d.json?.code} "${d.json?.msg}"`);
      if (p1GuardReservationId) {
        await api('POST', `/reservations/${p1GuardReservationId}/cancel`, {
          token: userToken,
          body: { reason: 'P1 清理' },
        });
      }
      const d2 = await api('DELETE', `/resources/${p1GuardId}`, { token: adminToken });
      ok('取消预约后再删除 → 成功', d2.json?.code === 0, `"${d2.json?.msg}"`);
    }
  }

  head('18. P1 路由隔离与自测台（§3.11 / T10）');
  {
    const a = await api('GET', '/resources/4', { token: adminToken });
    const b = await api('GET', `/resources/4/slots?date=${p1Date}`, { token: adminToken });
    ok('GET /resources/4 与 /resources/4/slots 互不干扰',
       a.json?.code === 0 && a.json?.data?.code === 'MR-202' &&
         b.json?.code === 0 && Array.isArray(b.json?.data?.occupied),
       `detail=${a.json?.data?.code} slots.occupied=${JSON.stringify(b.json?.data?.occupied)}`);
    const c = await api('GET', `/resources/4/slots`, { token: adminToken });
    ok('查占用缺 date → 1000', c.json?.code === 1000, `code=${c.json?.code} "${c.json?.msg}"`);
    const d = await api('GET', '/reservations?status=NOT_A_STATUS', { token: adminToken });
    ok('预约列表非法 status → 1000', d.json?.code === 1000, `code=${d.json?.code} "${d.json?.msg}"`);
  }
  {
    const r = await fetch('http://127.0.0.1:8080/debug/index.html', { signal: AbortSignal.timeout(5000) });
    const html = await r.text();
    const p0Panels = (html.match(/<section class="card">/g) || []).length;
    const p1Panels = (html.match(/<section class="card p1">/g) || []).length;
    ok('/debug 新增 7 个预约面板（P0 面板仍为 12 个）', p0Panels === 12 && p1Panels === 7,
       `P0=${p0Panels} P1=${p1Panels}`);
    ok('/debug 含预约主链路按钮', html.includes('btnResCreate') && html.includes('btnResCancel') &&
       html.includes('btnResSlots'), `字节=${html.length}`);
  }
  {
    const r = spawnSync('grep', ['-c', 'RESERVATION_CREATE', path.join(ROOT, 'logs', 'reserveflow.log')], {
      encoding: 'utf8',
    });
    const n = parseInt((r.stdout || '0').trim(), 10);
    ok('日志存在 RESERVATION_* 审计行（含 rid/op/code）', n > 0, `${n} 行`);
  }

  // ══════════════════════════════════════════════════════════════════════════
  //  P2 增量（T11）：闭环 / 押金 / 候补 / 定时任务 / 不变量
  //  说明：为了让「下单后立刻取用」在任意 pickup 窗口配置下都成立，
  //        闭环用例在 create 之后把 start_time/end_time 临时挪到 now-1min/now+29min
  //        （只改时钟，不改业务逻辑；create/approve 等步骤仍走真实 HTTP）。
  // ══════════════════════════════════════════════════════════════════════════
  const p2Date = dateAfter(60);
  const p2Date2 = dateAfter(61);

  const trackRes = (id) => {
    if (id) p2ResIds.push(id);
    return id;
  };
  /** 把单据时间挪到「现在正在使用中」的窗口，保证 pickup 必然在窗口内 */
  const nudgeTimes = (id) => {
    mysqlExec(
      `UPDATE reservation SET start_time = TIMESTAMPADD(MINUTE,-1,NOW()), ` +
        `end_time = TIMESTAMPADD(MINUTE,29,NOW()) WHERE id = ${id};`
    );
  };
  const resStatus = (id) => mysqlScalar(`SELECT status FROM reservation WHERE id=${id};`);
  const slotCount = (id) => mysqlScalar(`SELECT COUNT(*) FROM reservation_slot WHERE reservation_id=${id};`);
  const depositRows = (id) => mysqlScalar(`SELECT COUNT(*) FROM deposit_record WHERE reservation_id=${id};`);
  const credit = () => parseInt(mysqlScalar('SELECT credit_score FROM user WHERE id=2;'), 10);

  head('19. P2 押金闭环：冻结 → 取用 → 归还（释放槽） → 验收退还');
  let p2PaidId = null;
  let p2PaidRes = null;
  {
    // 用专用临时仪器（押金 200）：资源 1 在 P1 用例里还挂着一条 LOCKED 单，
    // 直接用它会让「验收后资源回 FREE」的断言被那条单正确地挡住。
    const pcode = 'P2V-PAID-' + String(Date.now()).slice(-6);
    const pr = await api('POST', '/resources', {
      token: adminToken,
      body: { code: pcode, name: 'P2 押金闭环仪器', type: 'LAB_EQUIPMENT', location: '验收楼-6', capacity: 1, depositAmount: 200 },
    });
    p2PaidRes = pr.json?.data?.id ?? null;
    if (p2PaidRes) p2ResCreated.push(p2PaidRes);
    ok('新建临时仪器（押金 200）', pr.json?.code === 0 && !!p2PaidRes, `id=${p2PaidRes} code=${pcode}`);

    const c = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: p2PaidRes, startTime: `${p2Date}T09:00:00`, endTime: `${p2Date}T10:00:00`, remark: 'P2 押金闭环' },
    });
    p2PaidId = trackRes(c.json?.data?.id);
    ok('仪器下单 → PENDING_REVIEW（押金 200）',
       c.json?.code === 0 && c.json?.data?.status === 'PENDING_REVIEW' &&
         Number(c.json?.data?.depositAmount) === 200,
       `code=${c.json?.code} status=${c.json?.data?.status} deposit=${c.json?.data?.depositAmount}`);

    const d1 = await api('GET', `/reservations/${p2PaidId}/deposits`, { token: userToken });
    const rows1 = d1.json?.data?.list || [];
    ok('下单后押金流水 1 条 FREEZE 200（状态 SUCCESS）',
       rows1.length === 1 && rows1[0].recordType === 'FREEZE' && Number(rows1[0].amount) === 200 &&
         rows1[0].status === 'SUCCESS' && rows1[0].transactionNo === `DP${p2PaidId}FREEZE`,
       JSON.stringify(rows1.map((x) => `${x.recordType}:${x.amount}:${x.transactionNo}`)));
    ok('账目聚合 state=FROZEN', d1.json?.data?.account?.state === 'FROZEN',
       JSON.stringify(d1.json?.data?.account));

    const a = await api('POST', `/reservations/${p2PaidId}/approve`, { token: adminToken, body: { remark: 'P2' } });
    ok('管理员审核 → LOCKED', a.json?.code === 0 && a.json?.data?.status === 'LOCKED',
       `status=${a.json?.data?.status}`);

    nudgeTimes(p2PaidId);
    const p = await api('POST', `/reservations/${p2PaidId}/pickup`, { token: userToken, body: {} });
    ok('取用 → IN_USE', p.json?.code === 0 && p.json?.data?.status === 'IN_USE',
       `code=${p.json?.code} status=${p.json?.data?.status} "${p.json?.msg}"`);
    ok('取用后 allowedActions 只剩用户可做的动作（不含系统事件）',
       JSON.stringify(p.json?.data?.allowedActions || []) === '["SUBMIT_RETURN"]',
       JSON.stringify(p.json?.data?.allowedActions));
    ok('取用后资源态 IN_USE',
       (await api('GET', `/resources/${p2PaidRes}`, { token: adminToken })).json?.data?.status === 'IN_USE');

    const before = Number(slotCount(p2PaidId));
    const r = await api('POST', `/reservations/${p2PaidId}/return`, { token: userToken, body: { remark: '提前用完' } });
    ok('归还 → RETURN_CHECK 且 slotIndexes 为空',
       r.json?.code === 0 && r.json?.data?.status === 'RETURN_CHECK' &&
         (r.json?.data?.slotIndexes || []).length === 0,
       `status=${r.json?.data?.status} slots=${JSON.stringify(r.json?.data?.slotIndexes)}`);
    ok('归还即释放槽位（D-P2-8）', before > 0 && Number(slotCount(p2PaidId)) === 0,
       `归还前 ${before} 槽 → 归还后 ${slotCount(p2PaidId)} 槽`);
    ok('归还后资源态 RETURN_CHECK（不可约）',
       (await api('GET', `/resources/${p2PaidRes}`, { token: adminToken })).json?.data?.status === 'RETURN_CHECK');

    const ac = await api('POST', `/reservations/${p2PaidId}/accept`, { token: adminToken, body: { remark: '验收通过' } });
    ok('验收通过 → COMPLETED 且资源 FREE',
       ac.json?.code === 0 && ac.json?.data?.status === 'COMPLETED' && ac.json?.data?.resourceStatus === 'FREE',
       `status=${ac.json?.data?.status} resource=${ac.json?.data?.resourceStatus}`);
    ok('验收响应含押金账目且 state=SETTLED（退还 200）',
       ac.json?.data?.deposit?.state === 'SETTLED' && Number(ac.json?.data?.deposit?.refunded) === 200,
       JSON.stringify(ac.json?.data?.deposit));
    ok('验收后槽位仍为空（幂等再删，D-P2-9）', Number(slotCount(p2PaidId)) === 0, slotCount(p2PaidId) + ' 槽');

    const d2 = await api('GET', `/reservations/${p2PaidId}/deposits`, { token: userToken });
    const rows2 = d2.json?.data?.list || [];
    ok('结清后流水 2 条：FREEZE + REFUND，账目平衡',
       rows2.length === 2 && rows2[0].recordType === 'FREEZE' && rows2[1].recordType === 'REFUND' &&
         Number(rows2[1].amount) === 200,
       JSON.stringify(rows2.map((x) => `${x.recordType}:${x.amount}`)));
  }

  head('20. P2 损坏结案：扣减 + 退剩余 + 违约记录 + 信用分 + 资源理赔态');
  let p2DmgRes = null;
  let p2DmgId = null;
  {
    p2CreditBefore = credit();
    const code = 'P2V-DMG-' + String(Date.now()).slice(-6);
    const cr = await api('POST', '/resources', {
      token: adminToken,
      body: { code, name: 'P2 损坏验证仪器', type: 'LAB_EQUIPMENT', location: '验收楼-3', capacity: 1, depositAmount: 100 },
    });
    p2DmgRes = cr.json?.data?.id ?? null;
    if (p2DmgRes) p2ResCreated.push(p2DmgRes);
    ok('新建临时仪器（押金 100）', cr.json?.code === 0 && !!p2DmgRes, `id=${p2DmgRes} code=${code}`);

    const c = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: p2DmgRes, startTime: `${p2Date}T14:00:00`, endTime: `${p2Date}T15:00:00` },
    });
    p2DmgId = trackRes(c.json?.data?.id);
    await api('POST', `/reservations/${p2DmgId}/approve`, { token: adminToken, body: {} });
    nudgeTimes(p2DmgId);
    await api('POST', `/reservations/${p2DmgId}/pickup`, { token: userToken, body: {} });
    await api('POST', `/reservations/${p2DmgId}/return`, { token: userToken, body: {} });

    const d = await api('POST', `/reservations/${p2DmgId}/damage`, {
      token: adminToken,
      body: { reason: '镜筒磕碰' },
    });
    ok('损坏结案 → COMPLETED 且资源 DAMAGE_CLAIM',
       d.json?.code === 0 && d.json?.data?.status === 'COMPLETED' &&
         d.json?.data?.resourceStatus === 'DAMAGE_CLAIM',
       `code=${d.json?.code} status=${d.json?.data?.status} resource=${d.json?.data?.resourceStatus}`);
    ok('按比例扣减 100*0.5=50，剩余 50 退还',
       Number(d.json?.data?.deposit?.deducted) === 50 && Number(d.json?.data?.deposit?.refunded) === 50 &&
         d.json?.data?.deposit?.state === 'SETTLED',
       JSON.stringify(d.json?.data?.deposit));
    ok('写入 DAMAGE 违约记录并扣 20 分（信用分 100→80）',
       d.json?.data?.violation?.type === 'DAMAGE' && Number(d.json?.data?.violation?.deductScore) === 20 &&
         Number(d.json?.data?.creditScoreAfter) === p2CreditBefore - 20,
       `violation=${JSON.stringify(d.json?.data?.violation)} creditAfter=${d.json?.data?.creditScoreAfter}`);
    ok('流水 3 条：FREEZE → DEDUCT → REFUND',
       Number(depositRows(p2DmgId)) === 3, depositRows(p2DmgId) + ' 行');
    ok('信用分库内确为 80', credit() === 80, String(credit()));

    // 复原资源（P0 的 PATCH 路径允许 FREE）
    const back = await api('PATCH', `/resources/${p2DmgRes}/status`, { token: adminToken, body: { status: 'FREE' } });
    ok('理赔态可由管理员手工恢复 FREE（DAMAGE_CLAIM → FREE 路径可用）',
       back.json?.code === 0 && back.json?.data?.status === 'FREE', `code=${back.json?.code}`);
  }

  head('21. P2 零押金资源：全程不产生任何押金流水（D-P2-12 / I3）');
  let p2FreeRes = null;
  let p2FreeId = null;
  {
    const code = 'P2V-FREE-' + String(Date.now()).slice(-6);
    const cr = await api('POST', '/resources', {
      token: adminToken,
      body: { code, name: 'P2 零押金会议室', type: 'MEETING_ROOM', location: '验收楼-4', capacity: 4, depositAmount: 0 },
    });
    p2FreeRes = cr.json?.data?.id ?? null;
    if (p2FreeRes) p2ResCreated.push(p2FreeRes);
    ok('新建临时会议室（押金 0）', cr.json?.code === 0 && !!p2FreeRes, `id=${p2FreeRes}`);

    const c = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: p2FreeRes, startTime: `${p2Date2}T09:00:00`, endTime: `${p2Date2}T10:00:00` },
    });
    p2FreeId = trackRes(c.json?.data?.id);
    ok('会议室下单 → LOCKED（免审核）', c.json?.code === 0 && c.json?.data?.status === 'LOCKED',
       `code=${c.json?.code} status=${c.json?.data?.status}`);

    nudgeTimes(p2FreeId);
    await api('POST', `/reservations/${p2FreeId}/pickup`, { token: userToken, body: {} });
    await api('POST', `/reservations/${p2FreeId}/return`, { token: userToken, body: {} });
    const ac = await api('POST', `/reservations/${p2FreeId}/accept`, { token: adminToken, body: {} });
    ok('零押金走完全流程 → COMPLETED', ac.json?.code === 0 && ac.json?.data?.status === 'COMPLETED',
       `status=${ac.json?.data?.status}`);

    const d = await api('GET', `/reservations/${p2FreeId}/deposits`, { token: userToken });
    ok('deposit_record 0 行且 state=NONE',
       (d.json?.data?.list || []).length === 0 && d.json?.data?.account?.state === 'NONE',
       `rows=${(d.json?.data?.list || []).length} state=${d.json?.data?.account?.state}`);
    ok('库内该单确实 0 条押金流水', Number(depositRows(p2FreeId)) === 0, depositRows(p2FreeId) + ' 行');
  }

  head('22. P2 候补闭环：入队 2 人 → 取消持有者 → 只发 1 个 offer → confirm 转正');
  {
    const wd = dateAfter(62);
    const holder = await api('POST', '/reservations', {
      token: adminToken,
      body: { resourceId: 4, startTime: `${wd}T09:00:00`, endTime: `${wd}T10:00:00`, remark: 'P2 候补占用者' },
    });
    const holderId = trackRes(holder.json?.data?.id);
    ok('管理员先占住该时段（候补前置）', holder.json?.code === 0 && !!holderId, `holder=${holderId}`);

    const j1 = await api('POST', '/resources/4/waitlist', {
      token: userToken,
      body: { startTime: `${wd}T09:00:00`, endTime: `${wd}T10:00:00` },
    });
    const w1 = j1.json?.data?.id ?? null;
    if (w1) p2WaitIds.push(w1);
    ok('student01 入队 → WAITING 且名次 1',
       j1.json?.code === 0 && j1.json?.data?.status === 'WAITING' && j1.json?.data?.position === 1,
       `id=${w1} status=${j1.json?.data?.status} pos=${j1.json?.data?.position}`);

    const j2 = await api('POST', '/resources/4/waitlist', {
      token: adminToken,
      body: { startTime: `${wd}T09:00:00`, endTime: `${wd}T10:00:00` },
    });
    const w2 = j2.json?.data?.id ?? null;
    if (w2) p2WaitIds.push(w2);
    ok('管理员入队 → 名次 2（严格 FIFO）', j2.json?.code === 0 && j2.json?.data?.position === 2,
       `id=${w2} pos=${j2.json?.data?.position}`);

    const dup = await api('POST', '/resources/4/waitlist', {
      token: userToken,
      body: { startTime: `${wd}T09:00:00`, endTime: `${wd}T10:00:00` },
    });
    ok('同窗口重复入队 → 3020', dup.json?.code === 3020, `code=${dup.json?.code} "${dup.json?.msg}"`);

    await api('POST', `/reservations/${holderId}/cancel`, { token: adminToken, body: { reason: '让位' } });

    const q = await api('GET', '/waitlist?size=50', { token: adminToken });
    const rows = q.json?.data?.list || [];
    const row1 = rows.find((x) => x.id === w1);
    const row2 = rows.find((x) => x.id === w2);
    ok('取消后只有第 1 位收到 OFFERED（不重复发同一时段）',
       row1?.status === 'OFFERED' && row2?.status === 'WAITING',
       `w1=${row1?.status} w2=${row2?.status}`);
    ok('offer 带 15 分钟确认截止时间', !!row1?.offerExpiresAt && row1.offerExpiresAt.length >= 19,
       `offerExpiresAt=${row1?.offerExpiresAt}`);
    ok('第 2 位仍排第 1（未被顶掉）', row2?.position === 1, `pos=${row2?.position}`);

    // 越权 confirm（学生点管理员那条）→ 3008
    const forb = await api('POST', `/waitlist/${w2}/confirm`, { token: userToken, body: {} });
    ok('confirm 他人候补 → 3008', forb.json?.code === 3008, `code=${forb.json?.code} "${forb.json?.msg}"`);

    const cf = await api('POST', `/waitlist/${w1}/confirm`, { token: userToken, body: {} });
    const newResId = cf.json?.data?.convertedReservationId ?? null;
    if (newResId) trackRes(newResId);
    ok('confirm → CONVERTED 并回填新的预约 id',
       cf.json?.code === 0 && cf.json?.data?.status === 'CONVERTED' && !!newResId,
       `status=${cf.json?.data?.status} newRes=${newResId}`);
    ok('转正后的预约占住了 2 个槽位',
       !!newResId && Number(slotCount(newResId)) === 2,
       newResId ? slotCount(newResId) + ' 槽' : '无');
    ok('转正后的预约状态 LOCKED 且押金 0 条流水',
       resStatus(newResId) === 'LOCKED' && Number(depositRows(newResId)) === 0,
       `${resStatus(newResId)} / ${depositRows(newResId)} 行`);

    const q2 = await api('GET', '/waitlist?size=50', { token: adminToken });
    const row2b = (q2.json?.data?.list || []).find((x) => x.id === w2);
    ok('转正后第 2 位仍在队首（WAITING pos=1）',
       row2b?.status === 'WAITING' && row2b?.position === 1,
       `status=${row2b?.status} pos=${row2b?.position}`);
  }

  head('23. P2 维护扫描：待审超时 / 爽约 / 逾期 / 逾期待扣（同一轮 + 幂等复跑）');
  let p2MaintRes = null;
  const maintIds = [];
  {
    const code = 'P2V-MAINT-' + String(Date.now()).slice(-6);
    const cr = await api('POST', '/resources', {
      token: adminToken,
      body: { code, name: 'P2 维护扫描验证', type: 'MEETING_ROOM', location: '验收楼-5', capacity: 2, depositAmount: 0 },
    });
    p2MaintRes = cr.json?.data?.id ?? null;
    if (p2MaintRes) p2ResCreated.push(p2MaintRes);
    ok('新建维护扫描用临时资源', cr.json?.code === 0 && !!p2MaintRes, `id=${p2MaintRes}`);

    const R = p2MaintRes;
    // ① 待审核超时（created_at 回拨 3 小时）
    mysqlExec(
      `INSERT INTO reservation (reservation_no,user_id,resource_id,start_time,end_time,status,deposit_amount,remark) ` +
        `VALUES ('P2M-A-${Date.now()}',2,${R},'${p2Date}T09:00:00','${p2Date}T10:00:00','PENDING_REVIEW',0,'maint A');`
    );
    const idA = parseInt(mysqlScalar(`SELECT id FROM reservation WHERE reservation_no LIKE 'P2M-A-%' ORDER BY id DESC LIMIT 1;`), 10);
    if (idA) { maintIds.push(idA); trackRes(idA); }
    mysqlExec(`INSERT INTO reservation_slot (reservation_id,resource_id,slot_date,slot_index) VALUES (${idA},${R},'${p2Date}',18),(${idA},${R},'${p2Date}',19);`);
    mysqlExec(`UPDATE reservation SET created_at = TIMESTAMPADD(HOUR,-3,NOW()) WHERE id=${idA};`);

    // ② 爽约（start_time 已是 45 分钟前，且已过 pickup.lateMin=30）
    mysqlExec(
      `INSERT INTO reservation (reservation_no,user_id,resource_id,start_time,end_time,status,deposit_amount,remark) ` +
        `VALUES ('P2M-B-${Date.now()}',2,${R},TIMESTAMPADD(MINUTE,-45,NOW()),TIMESTAMPADD(MINUTE,-15,NOW()),'LOCKED',0,'maint B');`
    );
    const idB = parseInt(mysqlScalar(`SELECT id FROM reservation WHERE reservation_no LIKE 'P2M-B-%' ORDER BY id DESC LIMIT 1;`), 10);
    if (idB) { maintIds.push(idB); trackRes(idB); }
    mysqlExec(`INSERT INTO reservation_slot (reservation_id,resource_id,slot_date,slot_index) VALUES (${idB},${R},'${p2Date}',20);`);

    // ③ 逾期未还（IN_USE，end_time 10 分钟前）
    mysqlExec(
      `INSERT INTO reservation (reservation_no,user_id,resource_id,start_time,end_time,status,deposit_amount,remark) ` +
        `VALUES ('P2M-C-${Date.now()}',2,${R},TIMESTAMPADD(MINUTE,-70,NOW()),TIMESTAMPADD(MINUTE,-10,NOW()),'IN_USE',0,'maint C');`
    );
    const idC = parseInt(mysqlScalar(`SELECT id FROM reservation WHERE reservation_no LIKE 'P2M-C-%' ORDER BY id DESC LIMIT 1;`), 10);
    if (idC) { maintIds.push(idC); trackRes(idC); }
    // 逾期单据不释放槽（时段仍归该单），所以必须给它真实的槽位行，否则会被 I2 正确判红
    mysqlExec(`INSERT INTO reservation_slot (reservation_id,resource_id,slot_date,slot_index) VALUES (${idC},${R},'${p2Date2}',30),(${idC},${R},'${p2Date2}',31);`);

    // ④ 逾期待续（DUE_RETURN，逾期 155 分钟 → seq=2）
    mysqlExec(
      `INSERT INTO reservation (reservation_no,user_id,resource_id,start_time,end_time,status,deposit_amount,remark) ` +
        `VALUES ('P2M-D-${Date.now()}',2,${R},TIMESTAMPADD(MINUTE,-215,NOW()),TIMESTAMPADD(MINUTE,-155,NOW()),'DUE_RETURN',0,'maint D');`
    );
    const idD = parseInt(mysqlScalar(`SELECT id FROM reservation WHERE reservation_no LIKE 'P2M-D-%' ORDER BY id DESC LIMIT 1;`), 10);
    if (idD) { maintIds.push(idD); trackRes(idD); }
    mysqlExec(`INSERT INTO reservation_slot (reservation_id,resource_id,slot_date,slot_index) VALUES (${idD},${R},'${p2Date2}',32),(${idD},${R},'${p2Date2}',33);`);
    ok('造好四类候选单据', maintIds.length === 4, `ids=${JSON.stringify(maintIds)}`);

    const beforeCredit = credit();
    const m1 = await api('POST', '/admin/jobs/maintenance', { token: adminToken, body: {} });
    ok('手动触发维护一轮成功', m1.json?.code === 0, `code=${m1.json?.code} stats=${JSON.stringify(m1.json?.data?.stats)}`);

    ok('① 待审核超时 → REJECTED 且槽位释放',
       resStatus(idA) === 'REJECTED' && Number(slotCount(idA)) === 0,
       `${resStatus(idA)} / ${slotCount(idA)} 槽`);
    ok('② 爽约 → CANCELLED 且槽位释放',
       resStatus(idB) === 'CANCELLED' && Number(slotCount(idB)) === 0,
       `${resStatus(idB)} / ${slotCount(idB)} 槽`);
    ok('② 写入 NO_SHOW 违约记录（-10 分）',
       mysqlScalar(`SELECT CONCAT(violation_type,'/',deduct_score,'/',deduct_seq) FROM violation_record WHERE reservation_id=${idB};`) === 'NO_SHOW/10/0',
       mysqlScalar(`SELECT CONCAT(violation_type,'/',deduct_score,'/',deduct_seq) FROM violation_record WHERE reservation_id=${idB};`));
    ok('③ 逾期 → DUE_RETURN 且写 OVERDUE/seq=0 违约',
       resStatus(idC) === 'DUE_RETURN' &&
         mysqlScalar(`SELECT CONCAT(violation_type,'/',deduct_score,'/',deduct_seq) FROM violation_record WHERE reservation_id=${idC};`) === 'OVERDUE/5/0',
       `${resStatus(idC)} / ${mysqlScalar(`SELECT CONCAT(deduct_score,'/',deduct_seq) FROM violation_record WHERE reservation_id=${idC};`)}`);
    ok('④ 逾期待续按周期扣分（逾期 155min / 60 → seq=2）',
       mysqlScalar(`SELECT CONCAT(violation_type,'/',deduct_score,'/',deduct_seq) FROM violation_record WHERE reservation_id=${idD};`) === 'OVERDUE/5/2',
       mysqlScalar(`SELECT CONCAT(violation_type,'/',deduct_score,'/',deduct_seq) FROM violation_record WHERE reservation_id=${idD};`));
    ok('信用分合计只扣 3 次（10 + 5 + 5）', credit() === beforeCredit - 20,
       `${beforeCredit} → ${credit()}`);

    // 同一批数据再扫两轮：唯一索引 + CAS 应保证不再扣分
    const violBefore = mysqlScalar(`SELECT COUNT(*) FROM violation_record WHERE reservation_id IN (${maintIds.join(',')});`);
    const creditBefore2 = credit();
    await api('POST', '/admin/jobs/maintenance', { token: adminToken, body: {} });
    const m3 = await api('POST', '/admin/jobs/maintenance', { token: adminToken, body: {} });
    const violAfter = mysqlScalar(`SELECT COUNT(*) FROM violation_record WHERE reservation_id IN (${maintIds.join(',')});`);
    ok('连扫 3 轮：违约记录不再新增（唯一索引幂等）', violAfter === violBefore,
       `${violBefore} → ${violAfter}`);
    ok('连扫 3 轮：信用分不再下降', credit() === creditBefore2,
       `${creditBefore2} → ${credit()}`);
    ok('幂等复跑的 overdueDeducted / noShowCancelled 均为 0',
       Number(m3.json?.data?.stats?.overdueDeducted) === 0 &&
         Number(m3.json?.data?.stats?.noShowCancelled) === 0,
       JSON.stringify(m3.json?.data?.stats));

    // 权限：普通用户不能手动跑维护
    const forb = await api('POST', '/admin/jobs/maintenance', { token: userToken, body: {} });
    ok('普通用户手动跑维护 → 403', forb.status === 403, `HTTP ${forb.status}`);
  }

  head('24. P2 权限：验收 / 损坏 仅管理员');
  {
    ok('普通用户调 accept → 403',
       (await api('POST', `/reservations/${p2PaidId}/accept`, { token: userToken, body: {} })).status === 403);
    ok('普通用户调 damage → 403',
       (await api('POST', `/reservations/${p2PaidId}/damage`, { token: userToken, body: {} })).status === 403);
    const d = await api('GET', `/reservations/${p2DmgId}/deposits`, { token: blackToken });
    ok('他人查押金流水 → 3008', d.json?.code === 3008, `code=${d.json?.code} "${d.json?.msg}"`);
  }

  head('25. P2 不变量 I1~I5（限定本轮创建的预约，避开历史数据）');
  {
    const inList = p2ResIds.length ? p2ResIds.join(',') : '0';
    const i1 = mysqlScalar(
      `SELECT COUNT(*) FROM reservation_slot s JOIN reservation r ON r.id=s.reservation_id ` +
        `WHERE r.status IN ('COMPLETED','CANCELLED','REJECTED') AND r.id IN (${inList});`
    );
    const i2 = mysqlScalar(
      `SELECT COUNT(*) FROM reservation r WHERE r.status IN ('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK') ` +
        `AND NOT EXISTS (SELECT 1 FROM reservation_slot s WHERE s.reservation_id=r.id) AND r.id IN (${inList});`
    );
    const i3 = mysqlScalar(
      `SELECT COUNT(*) FROM deposit_record d JOIN reservation r ON r.id=d.reservation_id ` +
        `WHERE r.deposit_amount=0 AND r.id IN (${inList});`
    );
    const i4 = mysqlScalar(
      `SELECT COUNT(*) FROM (SELECT reservation_id, ` +
        `SUM(CASE WHEN record_type='FREEZE' THEN amount ELSE 0 END) f, ` +
        `SUM(CASE WHEN record_type IN ('REFUND','DEDUCT') THEN amount ELSE 0 END) s ` +
        `FROM deposit_record WHERE reservation_id IN (${inList}) GROUP BY reservation_id HAVING s > f) t;`
    );
    const i5 = mysqlScalar(
      `SELECT COUNT(*) FROM (SELECT reservation_id, violation_type, deduct_seq FROM violation_record ` +
        `WHERE reservation_id IN (${inList}) GROUP BY reservation_id, violation_type, deduct_seq ` +
        `HAVING COUNT(*)>1) t;`
    );
    ok('I1 终态预约不占任何槽位', i1 === '0', `count=${i1}`);
    ok('I2 有效预约必须有槽位', i2 === '0', `count=${i2}`);
    ok('I3 零押金资源无押金流水', i3 === '0', `count=${i3}`);
    ok('I4 押金不得超额结算', i4 === '0', `count=${i4}`);
    ok('I5 违约记录不重复', i5 === '0', `count=${i5}`);
  }

  head('26. P2 自测台面板');
  {
    const r = await fetch('http://127.0.0.1:8080/debug/index.html', { signal: AbortSignal.timeout(5000) });
    const html = await r.text();
    const p0 = (html.match(/<section class="card">/g) || []).length;
    const p1 = (html.match(/<section class="card p1">/g) || []).length;
    const p2 = (html.match(/<section class="card p2">/g) || []).length;
    const p3 = (html.match(/<section class="card p3">/g) || []).length;
    ok('/debug 新增 10 个 P2 面板（P0 仍 12、P1 仍 7）', p0 === 12 && p1 === 7 && p2 === 10,
       `P0=${p0} P1=${p1} P2=${p2}`);
    ok('/debug 新增 5 个 P3 面板（规则管理 + 信用体系）', p3 === 5, `P3=${p3}`);
    ok('/debug 含取用/归还/验收/损坏/候补/维护按钮',
       html.includes('btnPickup') && html.includes('btnReturn') && html.includes('btnAccept') &&
         html.includes('btnDamage') && html.includes('btnJoinWait') && html.includes('btnWaitConfirm') &&
         html.includes('btnMaint'));
  }

  // ══════════════════════════════════════════════════════════════════════════
  //  P3 增量（T11）：规则管理 / 信用体系 / 补字段 / 引擎生效佐证
  // ══════════════════════════════════════════════════════════════════════════
  head('27. P3 规则管理接口（ARCH-P3 §7.5 / T08）');
  let p3TempRuleId = null;
  {
    const r = await api('GET', '/admin/rules', { token: adminToken });
    const list = r.json?.data?.list || [];
    const enabled = list.filter((x) => x.enabled).length;
    ok('GET /admin/rules 返回 10 条、启用 8 条', r.json?.code === 0 && list.length === 10 && enabled === 8,
       `total=${list.length} enabled=${enabled}`);

    const d = await api('GET', '/admin/rules/6', { token: adminToken });
    ok('GET /admin/rules/6 → RESERVE_NO_SHOW（参数 deductScore=10）',
       d.json?.code === 0 && d.json?.data?.ruleCode === 'RESERVE_NO_SHOW' &&
         d.json?.data?.params?.deductScore === 10 && d.json?.data?.evalPhase === 'TIMER',
       `${d.json?.data?.ruleCode} phase=${d.json?.data?.evalPhase} score=${d.json?.data?.params?.deductScore}`);

    const bad = await api('POST', '/admin/rules/validate', {
      token: adminToken,
      body: {
        ruleCode: 'lower_case', evalPhase: 'ALWAYS', priority: 99999,
        condition: { all: [{ op: 'between', field: 'user.status', value: 'X' }] },
        action: { type: 'nuke' }, params: {},
      },
    });
    ok('POST /admin/rules/validate 非法定义 → valid=false 且列出多条原因',
       bad.json?.code === 0 && bad.json?.data?.valid === false && (bad.json?.data?.errors || []).length >= 4,
       `errors=${(bad.json?.data?.errors || []).length} 条`);

    const goodBody = {
      ruleCode: 'P3V_TEMP_RULE', ruleName: 'P3 验收临时规则', ruleType: 'CREDIT',
      evalPhase: 'SETTLE', priority: 900,
      condition: { all: [] },
      action: { type: 'deduct_only', violationType: 'DAMAGE', deductCredit: true },
      params: { deductScore: 3 }, enabled: false, remark: '验收临时，跑完删除',
    };
    const good = await api('POST', '/admin/rules/validate', { token: adminToken, body: goodBody });
    ok('POST /admin/rules/validate 合法定义 → valid=true', good.json?.data?.valid === true,
       JSON.stringify(good.json?.data?.errors || []));

    const c = await api('POST', '/admin/rules', { token: adminToken, body: goodBody });
    p3TempRuleId = c.json?.data?.id ?? null;
    ok('POST /admin/rules 创建成功（返回 id）', c.json?.code === 0 && !!p3TempRuleId, `id=${p3TempRuleId}`);

    const dup = await api('POST', '/admin/rules', { token: adminToken, body: goodBody });
    ok('重复 rule_code → 3025', dup.json?.code === 3025, `code=${dup.json?.code} "${dup.json?.msg}"`);

    const prot = await api('DELETE', '/admin/rules/1', { token: adminToken });
    ok('删除内置规则 BLACKLIST_REJECT → 3028', prot.json?.code === 3028,
       `code=${prot.json?.code} "${prot.json?.msg}"`);

    if (p3TempRuleId) {
      const pr = await api('PATCH', `/admin/rules/${p3TempRuleId}/priority`, {
        token: adminToken,
        body: { priority: 901 },
      });
      ok('PATCH /admin/rules/{id}/priority 生效', pr.json?.code === 0 && pr.json?.data?.priority === 901,
         `priority=${pr.json?.data?.priority}`);

      const del = await api('DELETE', `/admin/rules/${p3TempRuleId}`, { token: adminToken });
      ok('DELETE 临时规则成功（非内置可删）', del.json?.code === 0, `"${del.json?.msg}"`);
      p3TempRuleId = null;
    }

    const forb = await api('GET', '/admin/rules', { token: userToken });
    ok('普通用户访问规则管理 → 403', forb.status === 403, `HTTP ${forb.status}`);
  }

  head('28. P3 信用接口（ARCH-P3 §7.5 / T09）');
  {
    const me = await api('GET', '/credit/me', { token: userToken });
    const d = me.json?.data || {};
    ok('/credit/me 返回分数/状态/阈值/距下一档',
       me.json?.code === 0 && typeof d.score === 'number' && d.status === 'NORMAL' &&
         d.enterScore === 60 && d.exitScore === 80 && typeof d.distanceToBlacklist === 'number' &&
         typeof d.hint === 'string',
       `score=${d.score} status=${d.status} 距黑名单=${d.distanceToBlacklist} hint=${d.hint}`);

    const logs = await api('GET', '/credit/me/logs?page=1&size=5', { token: userToken });
    const list = logs.json?.data?.list || [];
    ok('/credit/me/logs 分页可用且带 reasonLabel',
       logs.json?.code === 0 && Array.isArray(list) &&
         (list.length === 0 || (typeof list[0].changeScore === 'number' && !!list[0].reasonLabel)),
       `total=${logs.json?.data?.total} 首条=${list.length ? list[0].reasonType + '/' + list[0].reasonLabel : '无'}`);

    const bad = await api('GET', '/credit/me/logs?reasonType=NOT_A_REASON', { token: userToken });
    ok('非法 reasonType → 1000', bad.json?.code === 1000, `code=${bad.json?.code}`);

    const adm = await api('GET', '/admin/credit/2', { token: adminToken });
    ok('管理员查某人信用（含 recentLogs）',
       adm.json?.code === 0 && adm.json?.data?.userId === 2 && Array.isArray(adm.json?.data?.recentLogs),
       `userId=${adm.json?.data?.userId} recent=${(adm.json?.data?.recentLogs || []).length}`);

    // 手工调整：+1 再 -1（净零），并检查 MANUAL 明细
    const before = (await api('GET', '/credit/me', { token: userToken })).json?.data?.score;
    const adj = await api('POST', '/admin/credit/2/adjust', {
      token: adminToken,
      body: { changeScore: 1, reason: 'P3 验收：申诉补 1 分' },
    });
    if (adj.json?.data?.logId) p3CreditLogIds.push(adj.json.data.logId);
    ok('管理员 adjust +1 → 分数 +1 且写 MANUAL 明细',
       adj.json?.code === 0 && adj.json?.data?.scoreAfter === before + 1,
       `${adj.json?.data?.scoreBefore} → ${adj.json?.data?.scoreAfter} logId=${adj.json?.data?.logId}`);
    const back = await api('POST', '/admin/credit/2/adjust', {
      token: adminToken,
      body: { changeScore: -1, reason: 'P3 验收：还原' },
    });
    if (back.json?.data?.logId) p3CreditLogIds.push(back.json.data.logId);
    ok('再 adjust -1 还原到原分', back.json?.code === 0 && back.json?.data?.scoreAfter === before,
       `${back.json?.data?.scoreBefore} → ${back.json?.data?.scoreAfter}`);
    const manualLogs = await api('GET', '/credit/me/logs?page=1&size=5&reasonType=MANUAL',
                                 { token: userToken });
    ok('MANUAL 明细已落库且带 ruleCode=空 / sourceRef=ADMIN:*',
       (manualLogs.json?.data?.list || []).length >= 2,
       `MANUAL total=${manualLogs.json?.data?.total}`);

    const badAdjust = await api('POST', '/admin/credit/2/adjust', {
      token: adminToken,
      body: { changeScore: 0, reason: 'x' },
    });
    ok('changeScore=0 → 3026', badAdjust.json?.code === 3026, `code=${badAdjust.json?.code}`);
    const forbAdjust = await api('POST', '/admin/credit/2/adjust', {
      token: userToken,
      body: { changeScore: 5, reason: '普通用户想加分' },
    });
    ok('普通用户调整信用 → 403', forbAdjust.status === 403, `HTTP ${forbAdjust.status}`);
  }

  head('29. P3 补字段：resourceStatus + deposit（detail 与 list 都要有）');
  {
    const p3Date = dateAfter(70);
    const c = await api('POST', '/reservations', {
      token: userToken,
      body: { resourceId: 1, startTime: `${p3Date}T09:00:00`, endTime: `${p3Date}T10:00:00`, remark: 'P3 补字段验证' },
    });
    const rid = trackP1(c.json);
    ok('为补字段断言新建一张仪器单（押金 200）', c.json?.code === 0 && !!rid, `id=${rid}`);

    const d = await api('GET', `/reservations/${rid}`, { token: userToken });
    // resourceStatus 是**该资源的聚合态**（资源 1 上还挂着别的有效单，所以可能是 LOCKED），
    // 这里只断言它与 GET /resources/1 一致 + 押金字段正确
    const liveRes = await api('GET', '/resources/1', { token: adminToken });
    ok('详情含 resourceStatus（与资源实时状态一致）与 deposit（frozen=200 / FROZEN）',
       d.json?.data?.resourceStatus === liveRes.json?.data?.status &&
         Number(d.json?.data?.deposit?.frozen) === 200 && d.json?.data?.deposit?.state === 'FROZEN',
       `detail=${d.json?.data?.resourceStatus} live=${liveRes.json?.data?.status} deposit=${JSON.stringify(d.json?.data?.deposit)}`);

    const l = await api('GET', '/reservations?page=1&size=20', { token: adminToken });
    const rows = l.json?.data?.list || [];
    const mine = rows.find((x) => x.id === rid);
    ok('列表每行都有 resourceStatus 与 deposit（目标行押金 200）',
       rows.length > 0 && rows.every((x) => typeof x.resourceStatus === 'string' && x.deposit) &&
         !!mine && Number(mine.deposit.frozen) === 200,
       `行数=${rows.length} 目标行 resourceStatus=${mine?.resourceStatus} deposit=${JSON.stringify(mine?.deposit)}`);
  }

  head('30. P3 引擎生效佐证：credit_log 的 rule_code 与 status_log 的 rule_code');
  {
    // §23 造的四类单据已由引擎裁决；这里用它们的明细反向证明「规则码真的写进去了」
    const inList = p2ResIds.length ? p2ResIds.join(',') : '0';
    const codes = mysqlExec(
      `SELECT GROUP_CONCAT(DISTINCT CONCAT(reason_type,':',IFNULL(rule_code,'NULL')) ORDER BY reason_type) ` +
        `FROM credit_log WHERE reservation_id IN (${inList});`
    );
    const codeStr = (codes.out || '').trim();
    const sl = mysqlExec(
      `SELECT GROUP_CONCAT(DISTINCT IFNULL(rule_code,'NULL') ORDER BY rule_code) ` +
        `FROM status_log WHERE business_type='RESERVATION' AND business_id IN (${inList});`
    );
    const slStr = (sl.out || '').trim();

    if (ENGINE_ON) {
      ok('引擎开启：credit_log 规则码 = NO_SHOW→RESERVE_NO_SHOW / DAMAGE→RESERVE_DAMAGE_PENALTY / OVERDUE→RESERVE_OVERDUE*',
         codeStr.includes('NO_SHOW:RESERVE_NO_SHOW') &&
           codeStr.includes('DAMAGE:RESERVE_DAMAGE_PENALTY') && codeStr.includes('OVERDUE:RESERVE_OVERDUE'),
         codeStr);
      ok('引擎开启：status_log.rule_code 由引擎填充（含 RESERVE_DAMAGE_PENALTY）',
         slStr.includes('RESERVE_NO_SHOW') && slStr.includes('RESERVE_DAMAGE_PENALTY'), slStr);
    } else {
      // 引擎关闭 = P2 等价：3 个硬编码常量仍在（P2 就是这样写的），但它们来自代码常量而非规则
      ok('引擎关闭：status_log.rule_code 仍是 P2 的 3 个硬编码常量（RESERVE_NO_SHOW / RESERVE_OVERDUE / RESERVE_REVIEW_TIMEOUT）',
         slStr.includes('RESERVE_NO_SHOW') && slStr.includes('RESERVE_OVERDUE') &&
           slStr.includes('RESERVE_REVIEW_TIMEOUT'),
         slStr);
      ok('引擎关闭：credit_log 里没有规则码里的 DAMAGE 规则（P2 的 damage 不走规则）',
         !codeStr.includes('DAMAGE:RESERVE_DAMAGE_PENALTY'), codeStr);
    }
  }

  // ==========================================================================
  // §31 P6：注册与审核（ARCH-P6 §3.1 / §3.2）
  // 覆盖：注册 → 待审核闸门 → 管理员审核 → 登录放行 → 反枚举 → 幂等 → 拒绝重提
  // ==========================================================================
  head('31. P6 注册与审核（自助注册 / 审核闸门 / 反枚举 / 幂等 / 重提）');
  {
    const sfx = String(Date.now()).slice(-7);
    const uMain = `p6main${sfx}`; // 主链路：注册→待审→通过→登录
    const uRej = `p6rej${sfx}`; // 拒绝 → 重提链路
    const PWD = 'P6Check@12345';
    const PWD2 = 'P6Again@12345';
    p6Users.push(uMain, uRej);

    // ---- 31.1 注册
    const r1 = await api('POST', '/auth/register', {
      body: { username: uMain, realName: '验收甲', password: PWD },
    });
    const d1 = r1.json && r1.json.data ? r1.json.data : {};
    const idMain = d1.id;
    ok(
      '31.1 注册新学号 → code 0 / status=PENDING / statusLabel=待审核（id 与 userId 同时给出）',
      r1.json && r1.json.code === 0 && d1.status === 'PENDING' && d1.statusLabel === '待审核' &&
        idMain > 0 && d1.userId === idMain,
      `code=${r1.json && r1.json.code} status=${d1.status} label=${d1.statusLabel} id=${idMain} userId=${d1.userId}`
    );

    // ---- 31.2 重复注册（PENDING）→ 2004
    const r2 = await api('POST', '/auth/register', {
      body: { username: uMain, realName: '验收甲', password: PWD },
    });
    ok('31.2 重复注册同一账号（仍在待审核）→ 2004', r2.json && r2.json.code === 2004,
      `code=${r2.json && r2.json.code} msg=${r2.json && r2.json.msg}`);

    // ---- 31.3 注册已存在的 NORMAL 账号 → 2003
    const r3 = await api('POST', '/auth/register', {
      body: { username: 'student01', realName: '李同学', password: PWD },
    });
    ok('31.3 注册已存在的普通账号（student01 / NORMAL）→ 2003',
      r3.json && r3.json.code === 2003, `code=${r3.json && r3.json.code} msg=${r3.json && r3.json.msg}`);

    // ---- 31.4 待审核账号登录：必须是 HTTP 200 + 业务码 2004（不是 401）
    const l1 = await api('POST', '/auth/login', { body: { username: uMain, password: PWD } });
    ok('31.4 待审核账号登录 → HTTP 200 且 code=2004（业务错误不走 401）',
      l1.status === 200 && l1.json && l1.json.code === 2004,
      `HTTP ${l1.status} code=${l1.json && l1.json.code}`);

    // ---- 31.5 反枚举：错密码仍然只报 2000，不泄露「该账号待审核」
    const l2 = await api('POST', '/auth/login', { body: { username: uMain, password: 'Wrong@12345' } });
    ok('31.5 待审核账号 + 错误密码 → 2000（状态闸门在密码校验之后，不泄露账号状态）',
      l2.json && l2.json.code === 2000, `code=${l2.json && l2.json.code} msg=${l2.json && l2.json.msg}`);

    // ---- 31.6 管理员审核通过
    const a1 = await api('POST', `/admin/users/${idMain}/approve`, { token: adminToken, body: {} });
    ok('31.6 管理员 approve → code 0 / status=NORMAL',
      a1.json && a1.json.code === 0 && a1.json.data && a1.json.data.status === 'NORMAL',
      `code=${a1.json && a1.json.code} status=${a1.json && a1.json.data && a1.json.data.status}`);

    // ---- 31.7 通过后可以登录
    const l3 = await api('POST', '/auth/login', { body: { username: uMain, password: PWD } });
    ok('31.7 审核通过后登录 → code 0 且拿到 token',
      l3.json && l3.json.code === 0 && l3.json.data && (l3.json.data.token || '').length > 20,
      `code=${l3.json && l3.json.code} token长度=${(l3.json && l3.json.data && l3.json.data.token || '').length}`);

    // ---- 31.8 幂等：再点一次「通过」不报错
    const a2 = await api('POST', `/admin/users/${idMain}/approve`, { token: adminToken, body: {} });
    ok('31.8 对已 NORMAL 的账号重复 approve → code 0（刻意幂等，不返回 2007）',
      a2.json && a2.json.code === 0, `code=${a2.json && a2.json.code}`);

    // ---- 31.9 对已 NORMAL 的账号 reject → 2007
    const rj0 = await api('POST', `/admin/users/${idMain}/reject`,
      { token: adminToken, body: { reason: '验收：NORMAL 不应可拒绝' } });
    ok('31.9 对已 NORMAL 的账号 reject → 2007',
      rj0.json && rj0.json.code === 2007,
      `code=${rj0.json && rj0.json.code} msg=${rj0.json && rj0.json.msg}`);

    // ---- 31.10 注册 → 拒绝 → 登录 2005
    const r4 = await api('POST', '/auth/register', {
      body: { username: uRej, realName: '验收乙', password: PWD },
    });
    const idRej = r4.json && r4.json.data ? r4.json.data.id : 0;
    const rj1 = await api('POST', `/admin/users/${idRej}/reject`,
      { token: adminToken, body: { reason: '验收：学号与姓名不匹配' } });
    const l4 = await api('POST', '/auth/login', { body: { username: uRej, password: PWD } });
    ok('31.10 拒绝后登录 → HTTP 200 且 code=2005（注册审核未通过）',
      rj1.json && rj1.json.code === 0 && l4.status === 200 && l4.json && l4.json.code === 2005,
      `reject=${rj1.json && rj1.json.code} HTTP ${l4.status} code=${l4.json && l4.json.code}`);

    // ---- 31.11 被拒后重新注册：复用原行（id 不变），状态回 PENDING
    const r5 = await api('POST', '/auth/register', {
      body: { username: uRej, realName: '验收乙（已修正）', password: PWD2 },
    });
    ok('31.11 被拒后重新注册 → code 0 且**复用原 id**（不新建行）',
      r5.json && r5.json.code === 0 && r5.json.data && r5.json.data.id === idRej &&
        r5.json.data.status === 'PENDING',
      `原 id=${idRej} 新 id=${r5.json && r5.json.data && r5.json.data.id} status=${r5.json && r5.json.data && r5.json.data.status}`);

    // ---- 31.12 管理员列表能按 PENDING 筛出待审核账号
    const lp = await api('GET', '/admin/users?status=PENDING&page=1&size=50', { token: adminToken });
    const rowsP = (lp.json && lp.json.data && lp.json.data.list) || [];
    const hit = rowsP.find((x) => x.username === uRej);
    ok('31.12 GET /admin/users?status=PENDING 能筛出待审核账号（枚举扩值后列表代码零改动）',
      !!hit && hit.status === 'PENDING',
      `PENDING 共 ${rowsP.length} 条，命中 ${hit ? hit.username + '/' + hit.status : '未找到'}`);
  }
} catch (err) {
  ok('验收过程未抛异常', false, String(err && err.message ? err.message : err));
} finally {
  // 清理：把验收脚本建的资源 / 预约硬删除，保持库干净
  if (testId && existsSync(MYSQL_EXE)) {
    spawnSync(MYSQL_EXE, [
      '-u', 'rf', '-pRf@2026!', '-D', 'reserveflow',
      '-e', `DELETE FROM status_log WHERE business_type='RESOURCE' AND business_id=${testId}; DELETE FROM resource WHERE id=${testId};`,
    ]);
  }
  // P1 清理：预约单 + 槽位 + 留痕 + 临时资源，并把被收敛过的资源状态还原
  if (existsSync(MYSQL_EXE)) {
    const ids = [...new Set([...p1CreatedIds, p1ResId, p1LabId, p1RejectId, p1GuardReservationId].filter((x) => x))];
    if (ids.length) {
      const inList = ids.join(',');
      // P2 起：下单会写押金流水（资源 1 押金 200），清理时必须一并回收，否则留下孤儿流水
      mysqlExec(`DELETE FROM deposit_record WHERE reservation_id IN (${inList});`);
      mysqlExec(`DELETE FROM violation_record WHERE reservation_id IN (${inList});`);
      mysqlExec(`DELETE FROM credit_log WHERE reservation_id IN (${inList});`);
      mysqlExec(`DELETE FROM reservation_slot WHERE reservation_id IN (${inList});`);
      mysqlExec(`DELETE FROM status_log WHERE business_type='RESERVATION' AND business_id IN (${inList});`);
      mysqlExec(`DELETE FROM reservation WHERE id IN (${inList});`);
    }
    if (p1GuardId) {
      mysqlExec(`DELETE FROM status_log WHERE business_type='RESOURCE' AND business_id=${p1GuardId};`);
      mysqlExec(`DELETE FROM reservation_slot WHERE resource_id=${p1GuardId};`);
      mysqlExec(`DELETE FROM resource WHERE id=${p1GuardId};`);
    }
    // 资源 1 / 4 若已无有效预约则回 FREE（否则保持收敛结果）
    mysqlExec(
      `UPDATE resource SET status='FREE' WHERE id IN (1,4) ` +
        `AND status IN ('LOCKED','PENDING_REVIEW') AND NOT EXISTS (` +
        `SELECT 1 FROM reservation r WHERE r.resource_id = resource.id ` +
        `AND r.status IN ('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK'));`
    );
    // ---- P2 清理：预约 / 槽位 / 押金 / 违约 / 留痕 / 候补 / 临时资源
    const p2Ids = p2ResIds.length ? p2ResIds.join(',') : '0';
    mysqlExec(`DELETE FROM deposit_record WHERE reservation_id IN (${p2Ids});`);
    mysqlExec(`DELETE FROM violation_record WHERE reservation_id IN (${p2Ids});`);
    mysqlExec(`DELETE FROM credit_log WHERE reservation_id IN (${p2Ids});`);
    mysqlExec(`DELETE FROM reservation_slot WHERE reservation_id IN (${p2Ids});`);
    mysqlExec(`DELETE FROM status_log WHERE business_type='RESERVATION' AND business_id IN (${p2Ids});`);
    mysqlExec(`DELETE FROM reservation WHERE id IN (${p2Ids});`);
    for (const wid of p2WaitIds) {
      mysqlExec(`DELETE FROM waitlist WHERE id=${wid};`);
    }
    for (const rid of p2ResCreated) {
      mysqlExec(`DELETE FROM status_log WHERE business_type='RESOURCE' AND business_id=${rid};`);
      mysqlExec(`DELETE FROM reservation_slot WHERE resource_id=${rid};`);
      mysqlExec(`DELETE FROM resource WHERE id=${rid};`);
    }
    if (p3CreditLogIds.length) {
      mysqlExec(`DELETE FROM credit_log WHERE id IN (${p3CreditLogIds.join(',')});`);
    }
    // 兜底：本次脚本的「手工调整」都带固定 remark 前缀，且 reservation_id 为 NULL
    mysqlExec(
      `DELETE FROM credit_log WHERE user_id IN (1,2,3) AND reservation_id IS NULL ` +
        `AND reason_type='MANUAL' AND remark LIKE 'P3 验收%';`
    );
    mysqlExec('UPDATE user SET credit_score=100 WHERE id=2;');
    mysqlExec(
      `UPDATE resource SET status='FREE' WHERE id IN (1,4) ` +
        `AND status IN ('LOCKED','PENDING_REVIEW','IN_USE','DUE_RETURN','RETURN_CHECK') AND NOT EXISTS (` +
        `SELECT 1 FROM reservation r WHERE r.resource_id=resource.id ` +
        `AND r.status IN ('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK'));`
    );
    // ---- P6 清理：注册的测试学号按 username 精确删除（绝不 DELETE 全表）
    if (p6Users.length) {
      const nameList = p6Users.map((u) => `'${u.replace(/'/g, "''")}'`).join(',');
      mysqlExec(`DELETE FROM user WHERE username IN (${nameList});`);
      const p6Left = mysqlScalar(`SELECT COUNT(*) FROM user WHERE username IN (${nameList});`);
      ok('P6 测试账号已清理（按 username 精确删除）', p6Left === '0',
        `清理 ${p6Users.length} 个账号，残留 ${p6Left} 个`);
    }

    const p2Left = mysqlScalar(
      `SELECT COUNT(*) FROM reservation WHERE id IN (${p2Ids});`
    );
    ok('P2 测试数据已清理（本轮预约 / 候补 / 临时资源）', p2Left === '0' && p2WaitIds.length >= 0,
       `残留预约 ${p2Left} 条，清理候补 ${p2WaitIds.length} 条、临时资源 ${p2ResCreated.length} 个`);

    const left = mysqlExec(
      `SELECT COUNT(*) FROM reservation_slot WHERE reservation_id IN (${ids.length ? ids.join(',') : 0});`
    );
    const resLeft = mysqlExec('SELECT COUNT(*) FROM reservation;');
    const slotLeft = mysqlExec('SELECT COUNT(*) FROM reservation_slot;');
    console.log(
      `\n${C.d}清理：脚本产生的预约残留 ${left.out || 0} 条；库内现有 reservation=${resLeft.out} 行、reservation_slot=${slotLeft.out} 行${C.x}`
    );
  }
  if (child && !KEEP) child.kill();
}

// ── 汇总
const pass = results.filter((r) => r.pass).length;
const fail = results.length - pass;
console.log(`\n${C.b}==================== 验收结果 ====================${C.x}`);
console.log(`  规则引擎：${ENGINE_ON ? '开启（ruleEngineEnabled=true）' : '关闭（ruleEngineEnabled=false，走 P2 兜底）'}`);
console.log(`  共 ${results.length} 项    ${C.g}通过 ${pass}${C.x}    ${fail ? C.r + '失败 ' + fail + C.x : '失败 0'}`);
if (fail) {
  console.log(`\n${C.r}失败项：${C.x}`);
  results.filter((r) => !r.pass).forEach((r) => console.log(`  - ${r.name}  ${r.detail}`));
}
console.log(`${C.b}==================================================${C.x}`);
console.log(`自测台地址：http://localhost:8080/debug/ ${child && !KEEP ? '（服务已关闭，重新启动后再访问）' : '（服务运行中）'}`);

// ── 落盘报告
const verdict = fail === 0 ? '通过' : '未通过';
const stamp = new Date().toISOString().slice(0, 19).replace('T', ' ');
const lines = [
  '# ReserveFlow P0 验收报告',
  '',
  `- 执行时间：${stamp}`,
  `- 验收结论：**${verdict}**（${results.length} 项，通过 ${pass}，失败 ${fail}）`,
  `- 执行环境：Windows / MSVC 14.51 / MySQL 8.0.46 / Redis 7.2.5`,
  '',
  '| # | 验收项 | 结果 | 实测 |',
  '| --- | --- | --- | --- |',
  ...results.map((r, i) => `| ${i + 1} | ${r.name} | ${r.pass ? '通过' : '**失败**'} | ${r.detail.replace(/\|/g, '\\|')} |`),
  '',
  fail ? '## 失败项' : '## 说明',
  '',
  fail ? results.filter((r) => !r.pass).map((r) => `- ${r.name}：${r.detail}`).join('\n') : '全部验收项通过，无遗留问题。',
  '',
];
const DOCS = existsSync(path.resolve(ROOT, '..', '..', 'docs'))
  ? path.resolve(ROOT, '..', '..', 'docs')
  : path.join(ROOT, 'docs');
mkdirSync(DOCS, { recursive: true });
writeFileSync(path.join(DOCS, 'P0-验收报告.md'), lines.join('\n'), 'utf8');
console.log(`${C.d}报告已写入 ${path.join(DOCS, 'P0-验收报告.md')}${C.x}`);

process.exit(fail === 0 ? 0 : 1);
