#!/usr/bin/env node
/**
 * ReserveFlow P1 并发压测 —— 「同一资源、同一时段」极限抢占
 *
 *   node scripts/stress.mjs                                  # 默认 50 并发，资源 4，明天 09:00-10:00
 *   node scripts/stress.mjs --n 100 --resource 4 --date 2026-02-10 --start 09:00 --end 10:00
 *   node scripts/stress.mjs --n 100 --keep                   # 保留产生的预约，便于人工点 /debug 复核
 *
 * 断言（全部为硬断言，任一不满足即退出码 1）：
 *   ① 成功数**恰好 1**
 *   ② 失败码全部 ∈ {3004 资源正忙, 3005 时段被占用}
 *   ③ 直连 MySQL：该资源该时段「有效预约」条数 == 1
 *   ④ 直连 MySQL：该资源该日该时段槽位行数 == 槽数（没有多写、没有少写）
 *   ⑤ 直连 MySQL：该资源该日不存在重复 slot_index（唯一索引的独立性复核）
 *
 * 说明（ARCH-P1 §9-3）：当 --n 远大于锁串行化吞吐时，部分请求会以 3004 失败而非 3005，
 * 这**不是 bug**：3004 = 没抢到锁，3005 = 抢到锁但时段已被占。两者都算预期失败。
 * 真正的正确性来自 uk_slot_resource_date_index，而不是锁。
 *
 * 只依赖 Node 内置模块与 mysql.exe，不需要 npm install。
 */

import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EXE = [
  'rfbuild', 'cmake-build-msvc', 'build', 'build-clean', 'cmake-build-debug',
  'cmake-build-release', 'build/Debug', 'build/Release', 'cmake-build-debug/Debug',
]
  .map((d) => path.join(ROOT, d, 'reserveflow.exe'))
  .find((p) => existsSync(p)) || path.join(ROOT, 'build', 'reserveflow.exe');
const BASE = 'http://127.0.0.1:8080/api/v1';
const MYSQL_EXE = 'C:\\Program Files\\MySQL\\MySQL Server 8.0\\bin\\mysql.exe';
const MYSQL_ARGS = ['-u', 'rf', '-pRf@2026!', '-D', 'reserveflow', '-N', '-B'];

const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[90m', b: '\x1b[1m', x: '\x1b[0m' };

// ── 参数解析
function argOf(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}
function tomorrow() {
  const d = new Date(Date.now() + 24 * 3600 * 1000);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

const N = Math.max(1, parseInt(argOf('n', '50'), 10) || 50);
const RESOURCE = parseInt(argOf('resource', '4'), 10) || 4;
const DATE = argOf('date', tomorrow());
const START = argOf('start', '09:00');
const END = argOf('end', '10:00');
const USER = argOf('user', 'student01');
const PASSWORD = argOf('password', 'User@123456');
const KEEP = process.argv.includes('--keep');

const results = [];
let child = null;

function ok(name, pass, detail = '') {
  results.push({ name, pass, detail });
  const tag = pass ? `${C.g}PASS${C.x}` : `${C.r}FAIL${C.x}`;
  console.log(`  [${tag}] ${name}${detail ? `  ${C.d}${detail}${C.x}` : ''}`);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** "09:30" → 19（半点对齐才返回，否则 -1） */
function slotIndexOf(hhmm) {
  const m = /^(\d{1,2}):(\d{2})$/.exec(hhmm.trim());
  if (!m) return -1;
  const h = parseInt(m[1], 10);
  const mi = parseInt(m[2], 10);
  if (h < 0 || h > 23 || (mi !== 0 && mi !== 30)) return -1;
  return h * 2 + (mi === 30 ? 1 : 0);
}

function mysql(sql) {
  const r = spawnSync(MYSQL_EXE, [...MYSQL_ARGS, '-e', sql], { encoding: 'utf8' });
  if (r.status !== 0) {
    return { ok: false, out: ((r.stderr || '') + (r.stdout || '')).trim() };
  }
  return { ok: true, out: (r.stdout || '').trim() };
}

async function api(method, p, token, body) {
  const headers = { 'Content-Type': 'application/json' };
  if (token) headers.Authorization = `Bearer ${token}`;
  const res = await fetch(BASE + p, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    // 120s：高并发下 TCP 连接建立会排队（内核 listen backlog 有限），
    // 客户端超时过短会拿到 -1（不是业务失败码），从而污染「失败码 ∈ {3004,3005}」断言
    signal: AbortSignal.timeout(120000),
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

/**
 * 压测用的直连 HTTP 助手（node:http，**强制 Connection: close**）。
 *
 * 为什么不用 fetch 也不用 keep-alive：
 *   httplib 是「8 个 worker 线程 + 每条连接独占一个 worker」的模型。若客户端保持 100 条
 *   keep-alive 长连接，worker 会一直粘在已连接的前 8 条上，其余 92 条要等空闲连接超时
 *   （CPPHTTPLIB_KEEPALIVE_TIMEOUT_SEC，默认 5s）才能被服务，实测把 100 并发拖成 36~60s。
 *   每条请求用完即关，才是「100 个请求同时在飞」的真实吞吐（实测 ~350ms）。
 *   （浏览器同源并发只有 6 条，正常使用感知不到这个特性。）
 */
function httpJson(pathname, body, token) {
  return new Promise((resolve) => {
    const data = body === undefined ? '' : JSON.stringify(body);
    const headers = {
      'Content-Type': 'application/json',
      Connection: 'close',
    };
    if (data) headers['Content-Length'] = Buffer.byteLength(data);
    if (token) headers.Authorization = `Bearer ${token}`;
    const req = http.request(
      { host: '127.0.0.1', port: 8080, path: `/api/v1${pathname}`, method: 'POST', headers },
      (res) => {
        let text = '';
        res.on('data', (c) => (text += c));
        res.on('end', () => {
          let json = null;
          try {
            json = JSON.parse(text);
          } catch {
            json = null;
          }
          resolve({ status: res.statusCode, json, text });
        });
      }
    );
    req.on('error', (e) => resolve({ status: 0, json: { code: -1, msg: e.message }, text: '' }));
    req.end(data || undefined);
  });
}

async function alive() {
  try {
    const r = await api('GET', '/health/deps');
    return r.status === 200;
  } catch {
    return false;
  }
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

// ────────────────────────────────────────────────────────────
console.log(`${C.b}ReserveFlow P1 并发压测${C.x}   ${new Date().toLocaleString('zh-CN')}`);
console.log(`${C.d}目标：资源 ${RESOURCE} / ${DATE} ${START}-${END} / 并发 ${N}${C.x}`);

const startIndex = slotIndexOf(START);
const endIndex = slotIndexOf(END);
if (startIndex < 0 || endIndex < 0 || startIndex >= endIndex) {
  console.error(`${C.r}参数错误：--start / --end 必须是半点对齐的 HH:MM，且 start < end${C.x}`);
  process.exit(2);
}
const slotIndexes = [];
for (let i = startIndex; i < endIndex; i++) slotIndexes.push(i);
const startTime = `${DATE}T${START.padStart(5, '0')}:00`;
const endTime = `${DATE}T${END.padStart(5, '0')}:00`;

console.log(`\n${C.b}0. 前置${C.x}`);
ok('MySQL 3306 可连接', await tcp('127.0.0.1', 3306));
ok('Redis 6379 可连接', await tcp('127.0.0.1', 6379));

if (!(await alive())) {
  if (!existsSync(EXE)) {
    console.error(`${C.r}找不到 ${EXE}，请先构建${C.x}`);
    process.exit(2);
  }
  child = spawn(EXE, [], { cwd: ROOT, stdio: 'ignore' });
  let ready = false;
  for (let i = 0; i < 40; i++) {
    if (await alive()) {
      ready = true;
      break;
    }
    await sleep(500);
  }
  ok('启动 reserveflow.exe 并监听 8080', ready, ready ? EXE : '20 秒内未就绪');
  if (!ready) {
    child.kill();
    process.exit(3);
  }
} else {
  ok('8080 已有服务在跑，直接复用', true);
}

// ────────────────────────────────────────────────────────────
let createdId = null;
let beforeActive = 0;

try {
  console.log(`\n${C.b}1. 登录与压测前基线${C.x}`);
  const login = await httpJson('/auth/login', { username: USER, password: PASSWORD }, null);
  const token = login.json?.data?.token || '';
  ok(`账号 ${USER} 登录成功`, login.json?.code === 0 && token.length > 100, `token=${token.length} 字符`);
  if (!token) throw new Error('登录失败，无法继续');

  const base = mysql(
    `SELECT COUNT(*) FROM reservation WHERE resource_id=${RESOURCE} ` +
      `AND status IN ('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK') ` +
      `AND start_time < '${DATE} ${END}:00' AND end_time > '${DATE} ${START}:00';`
  );
  beforeActive = parseInt(base.out || '0', 10) || 0;
  ok('压测前该时段无有效预约（可重复跑）', beforeActive === 0, `已有 ${beforeActive} 条`);

  if (beforeActive > 0) {
    console.log(`${C.y}  该时段已被占用，先清掉旧数据再压测${C.x}`);
    mysql(
      `DELETE s FROM reservation_slot s JOIN reservation r ON r.id=s.reservation_id ` +
        `WHERE r.resource_id=${RESOURCE} AND r.start_time < '${DATE} ${END}:00' ` +
        `AND r.end_time > '${DATE} ${START}:00';`
    );
    mysql(
      `DELETE FROM reservation WHERE resource_id=${RESOURCE} ` +
        `AND start_time < '${DATE} ${END}:00' AND end_time > '${DATE} ${START}:00';`
    );
  }

  console.log(`\n${C.b}2. 并发抢占（${N} 个请求同时打同一时段）${C.x}`);
  const t0 = Date.now();
  const reqs = [];
  for (let i = 0; i < N; i++) {
    reqs.push(
      httpJson(
        '/reservations',
        { resourceId: RESOURCE, startTime, endTime, remark: `stress-${i + 1}` },
        token
      )
    );
  }
  const all = await Promise.all(reqs);
  const elapsed = Date.now() - t0;

  const success = all.filter((r) => r.json?.code === 0);
  const failByCode = new Map();
  for (const r of all) {
    const c = r.json?.code ?? -1;
    if (c !== 0) failByCode.set(c, (failByCode.get(c) || 0) + 1);
  }
  const failTotal = all.length - success.length;
  const failDist = [...failByCode.entries()]
    .sort((a, b) => b[1] - a[1])
    .map(([c, n]) => `${c}×${n}`)
    .join(' ');

  console.log(
    `  ${C.d}耗时 ${elapsed}ms，成功 ${success.length}，失败 ${failTotal}（${failDist || '无'}）${C.x}`
  );
  ok('成功数量恰好 1', success.length === 1, `成功 ${success.length} / 共 ${N}`);
  if (success.length === 1) {
    createdId = success[0].json?.data?.id ?? null;
    ok(
      '成功响应的预约状态为 LOCKED（会议室免审核）',
      success[0].json?.data?.status === 'LOCKED',
      `status=${success[0].json?.data?.status} no=${success[0].json?.data?.reservationNo}`
    );
  }
  const allowed = new Set([3004, 3005]);
  const badCodes = [...failByCode.keys()].filter((c) => !allowed.has(c));
  ok(
    '失败码全部 ∈ {3004 资源正忙, 3005 时段被占用}',
    badCodes.length === 0,
    badCodes.length === 0 ? failDist || '无失败' : `越界码 ${badCodes.join(',')}`
  );

  console.log(`\n${C.b}3. 直连 MySQL 断言（最终真相在唯一索引）${C.x}`);
  const activeSql = mysql(
    `SELECT COUNT(*) FROM reservation WHERE resource_id=${RESOURCE} ` +
      `AND status IN ('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK') ` +
      `AND start_time < '${DATE} ${END}:00' AND end_time > '${DATE} ${START}:00';`
  );
  const activeCount = parseInt(activeSql.out || '0', 10);
  ok('该资源该时段有效预约条数 == 1', activeCount === 1, `实际 ${activeCount} 条`);

  const slotSql = mysql(
    `SELECT COUNT(*) FROM reservation_slot WHERE resource_id=${RESOURCE} ` +
      `AND slot_date='${DATE}' AND slot_index IN (${slotIndexes.join(',')});`
  );
  const slotCount = parseInt(slotSql.out || '0', 10);
  ok(
    `槽位行数 == 槽数(${slotIndexes.length})（无多写/少写）`,
    slotCount === slotIndexes.length,
    `实际 ${slotCount} 行，槽 ${JSON.stringify(slotIndexes)}`
  );

  const dupSql = mysql(
    `SELECT COUNT(*) FROM (SELECT slot_index FROM reservation_slot WHERE resource_id=${RESOURCE} ` +
      `AND slot_date='${DATE}' GROUP BY slot_index HAVING COUNT(*) > 1) t;`
  );
  const dupCount = parseInt(dupSql.out || '0', 10);
  ok('该资源该日无重复 slot_index（唯一索引复核）', dupCount === 0, `重复槽数 ${dupCount}`);

  const resRow = mysql(`SELECT status FROM resource WHERE id=${RESOURCE};`);
  console.log(`  ${C.d}资源 ${RESOURCE} 当前状态：${resRow.out}${C.x}`);
} catch (err) {
  ok('压测过程未抛异常', false, String(err && err.message ? err.message : err));
} finally {
  if (!KEEP && createdId) {
    console.log(`\n${C.b}4. 清理${C.x}`);
    mysql(`DELETE FROM reservation_slot WHERE reservation_id=${createdId};`);
    mysql(
      `DELETE FROM status_log WHERE business_type='RESERVATION' AND business_id=${createdId};`
    );
    mysql(`DELETE FROM reservation WHERE id=${createdId};`);
    const left = mysql(
      `SELECT COUNT(*) FROM reservation WHERE resource_id=${RESOURCE} ` +
        `AND status IN ('PENDING_REVIEW','LOCKED','IN_USE','DUE_RETURN','RETURN_CHECK');`
    );
    const leftN = parseInt(left.out || '0', 10);
    if (leftN === 0) {
      mysql(`UPDATE resource SET status='FREE' WHERE id=${RESOURCE} AND status IN ('LOCKED','PENDING_REVIEW');`);
    }
    const check = mysql(`SELECT COUNT(*) FROM reservation WHERE id=${createdId};`);
    ok('压测产生的预约已清理', parseInt(check.out || '0', 10) === 0, `id=${createdId}`);
    console.log(`  ${C.d}资源 ${RESOURCE} 是否残留有效预约：${leftN === 0 ? '无（已回 FREE）' : leftN + ' 条'}${C.x}`);
  } else if (KEEP) {
    console.log(`\n${C.y}--keep：保留预约 id=${createdId}，可到 /debug 复核${C.x}`);
  }
  if (child) child.kill();
}

// ── 汇总
const pass = results.filter((r) => r.pass).length;
const fail = results.length - pass;
console.log(`\n${C.b}==================== 压测结果 ====================${C.x}`);
console.log(`  共 ${results.length} 项    ${C.g}通过 ${pass}${C.x}    ${fail ? C.r + '失败 ' + fail + C.x : '失败 0'}`);
if (fail) {
  console.log(`\n${C.r}失败项：${C.x}`);
  results.filter((r) => !r.pass).forEach((r) => console.log(`  - ${r.name}  ${r.detail}`));
}
console.log(`${C.b}==================================================${C.x}`);
process.exit(fail === 0 ? 0 : 1);
