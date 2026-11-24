// =============================================================================
// src/api/http.js  —  H5 版 Axios 实例与唯一错误出口
//
// 结构与 front/admin/src/api/http.js 完全同构，只换提示层：
//   ElMessage.error  →  showFailToast（移动端更合适：居中、2 秒、不挡 TabBar）
//
// 三条纪律（与 P4 一致）：
//   1) 只判 code，不判 HTTP 状态（后端 D-6：业务错误一律 HTTP 200 + code≠0）；
//      例外是 401（令牌失效）与 403（权限不足）。
//   2) 调用方不写 try/catch 弹提示 —— 全部在这里做。
//   3) 不 import store / router（避免循环依赖）：token 从 localStorage 读，
//      401 后的跳转由 main.js 用 setUnauthorizedHandler 注入。
// =============================================================================
import axios from 'axios'
import { showDialog, showFailToast } from 'vant'

import { TOKEN_KEY, USER_KEY } from '../constants/storage'
import { SPECIAL_CODE } from '../constants/errorCode'

const http = axios.create({
  baseURL: import.meta.env.VITE_API_BASE || '/api/v1',
  timeout: 15000,
  headers: { 'Content-Type': 'application/json' },
})

/** 401 之后的跳转动作，由 main.js 注入 */
let unauthorizedHandler = null
export function setUnauthorizedHandler(fn) {
  unauthorizedHandler = fn
}

/** 业务错误对象：页面可以 catch 到 code 做额外动作 */
export class BusinessError extends Error {
  constructor(code, msg, data) {
    super(msg)
    this.code = code
    this.msg = msg
    this.data = data
    this.business = true
  }
}

/**
 * 统一呈现：需要用户知晓的用 Dialog，其余用 Toast（ARCH-P5 §8）
 * @param {number} code
 * @param {string} msg 后端给的中文
 */
function present(code, msg) {
  const special = SPECIAL_CODE[code]
  const text = special?.tip || msg || '操作失败'
  if (special?.present === 'dialog') {
    showDialog({ title: '提示', message: text, confirmButtonText: '知道了' }).catch(() => {})
  } else {
    showFailToast(text)
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
    // 业务成功：直接把 data 交给调用方
    if (body && body.code === 0) {
      return body.data
    }
    const code = body?.code ?? -1
    const msg = body?.msg || '请求失败'
    // silent：调用方自己按码分流提示（如注册页 2003/2004/1000 三种文案各不相同）
    // —— 这是 opt-in 的，其它接口行为不变
    if (!res.config?.silent) {
      present(code, msg)
    }
    return Promise.reject(new BusinessError(code, msg, body?.data))
  },
  (err) => {
    const status = err.response?.status
    const body = err.response?.data

    // 令牌失效 → 清 token 并跳登录
    if (status === 401) {
      localStorage.removeItem(TOKEN_KEY)
      localStorage.removeItem(USER_KEY)
      showFailToast(body?.msg || '登录已失效，请重新登录')
      if (typeof unauthorizedHandler === 'function') unauthorizedHandler()
      return Promise.reject(new BusinessError(body?.code ?? 1001, '登录已失效'))
    }

    if (status === 403) {
      showFailToast(body?.msg || '无权访问')
      return Promise.reject(new BusinessError(body?.code ?? 1004, '无权访问'))
    }

    if (status === 400 || status === 404 || status === 405) {
      showFailToast(body?.msg || `请求异常（HTTP ${status}）`)
      return Promise.reject(new BusinessError(body?.code ?? -1, body?.msg || `HTTP ${status}`))
    }

    if (err.code === 'ECONNABORTED') {
      showFailToast('请求超时，请检查网络')
    } else if (!err.response) {
      showFailToast('无法连接服务器，请稍后重试')
    } else {
      showFailToast(body?.msg || `服务异常（HTTP ${status}）`)
    }
    return Promise.reject(new BusinessError(body?.code ?? -1, body?.msg || String(err.message)))
  }
)

/** 取某个错误码的额外动作（页面用：catch(e){ if (specialOf(e.code)?.action === 'refreshSlots') ... }） */
export function specialOf(code) {
  return SPECIAL_CODE[code] || null
}

export default http
