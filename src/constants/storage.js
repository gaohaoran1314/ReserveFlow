// =============================================================================
// localStorage 键名集中定义
// 为什么单独一个文件：http.js 需要读 token，auth store 需要读写 token；
// 若 http.js 直接 import store 会形成 store → api → http → store 的环形依赖。
// 把「键名」抽出来，两边都只依赖这个常量，环就断了。
// =============================================================================

export const TOKEN_KEY = 'rf_admin_token'
export const USER_KEY = 'rf_admin_user'
export const SIDEBAR_KEY = 'rf_admin_sidebar_collapsed'
