// =============================================================================
// src/api/http.js  —  Axios 实例与唯一错误出口
// 对应 ARCH-P4 §3.2 / §3.3 / D-P4-4
//
// 两条纪律：
//   1) **只判 code，不判 HTTP 状态**（后端 D-6：业务错误一律 HTTP 200 + code≠0）；
//      唯一例外是 401（令牌失效）与 403（权限不足），这两种是鉴权层的真实状态码。
//   2) 调用方不写 try/catch、不弹提示 —— 失败提示统一在这里做，
//      调用方只关心 `await xxx()` 拿到 data 或 catch 到 `{code,msg}`。
//
// 刻意不 import store / router（会形成环形依赖）：
//   · token 直接从 localStorage 读（与 auth store 共用 constants/storage.js 的键名）
//   · 401 后的跳转由 main.js 通过 setUnauthorizedHandler() 注入
// =============================================================================
import axios from 'axios'
import { ElMessage } from 'element-plus'

import { TOKEN_KEY, USER_KEY } from '../constants/storage'
import { SPECIAL_CODE } from '../constants/errorCode'

const http = axios.create({
  baseURL: import.meta.env.VITE_API_BASE || '/api/v1',
  timeout: 15000,
  headers: { 'Content-Type': 'application/json' },
})

/** 401 之后的跳转动作，由 main.js 注入（避免 http.js 依赖 router） */
let unauthorizedHandler = null
export function setUnauthorizedHandler(fn) {
  unauthorizedHandler = fn
}

/** 供调用方在特殊码（3005/3007 等）时做额外动作后重新抛出 */
export class BusinessError extends Error {
  constructor(code, msg, data) {
    super(msg)
    this.code = code
    this.msg = msg
    this.data = data
    this.business = true
  }
}

// ---------------------------------------------------------------- 请求拦截
http.interceptors.request.use((cfg) => {
  const token = localStorage.getItem(TOKEN_KEY)
  if (token) {
    cfg.headers.Authorization = `Bearer ${token}`
  }
  return cfg
})

// ---------------------------------------------------------------- 响应拦截
http.interceptors.response.use(
  (res) => {
    const body = res.data
    // 业务成功：直接把 data 交给调用方（调用方不需要再解一层）
    if (body && body.code === 0) {
      return body.data
    }

    // 业务失败：HTTP 200 + code≠0
    const code = body?.code ?? -1
    const msg = body?.msg || '请求失败'
    ElMessage.error(msg)
    return Promise.reject(new BusinessError(code, msg, body?.data))
  },
  (err) => {
    const status = err.response?.status
    const body = err.response?.data

    // ---- 令牌失效：清干净并跳登录 ----
    if (status === 401) {
      localStorage.removeItem(TOKEN_KEY)
      localStorage.removeItem(USER_KEY)
      if (typeof unauthorizedHandler === 'function') {
        unauthorizedHandler(body?.msg || '登录已失效，请重新登录')
      } else {
        ElMessage.warning(body?.msg || '登录已失效，请重新登录')
      }
      return Promise.reject(new BusinessError(body?.code ?? 1001, '登录已失效'))
    }

    // ---- 权限不足：只提示，不跳转（跳登录会误导成"密码错了"） ----
    if (status === 403) {
      ElMessage.error(body?.msg || '权限不足，需要管理员角色')
      return Promise.reject(new BusinessError(body?.code ?? 1004, '权限不足'))
    }

    if (status === 400 || status === 404 || status === 405) {
      ElMessage.error(body?.msg || `请求异常（HTTP ${status}）`)
      return Promise.reject(new BusinessError(body?.code ?? -1, body?.msg || `HTTP ${status}`))
    }

    // ---- 连接类错误：联调第一坑 ----
    if (err.code === 'ECONNABORTED') {
      ElMessage.error('请求超时，请检查后端服务是否正常')
    } else if (!err.response) {
      ElMessage.error('无法连接后端服务（127.0.0.1:8080），请确认已启动 reserveflow.exe')
    } else {
      ElMessage.error(body?.msg || `服务异常（HTTP ${status}）`)
    }
    return Promise.reject(new BusinessError(body?.code ?? -1, body?.msg || String(err.message)))
  }
)

/**
 * 判断某个错误码是否需要页面做额外动作（刷新时间轴 / 重拉详情等）。
 * 页面用法：`catch (e) { if (needAction(e.code)) { ... } }`
 */
export function specialActionOf(code) {
  return SPECIAL_CODE[code] || null
}

export default http
