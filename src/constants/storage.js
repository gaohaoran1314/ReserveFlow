// =============================================================================
// localStorage 键名（**必须与 admin 不同**）
//
// 为什么：两个前端可能部署在同一域名下（localhost:8080/admin 与 /h5），
// localStorage 是同源的。若都用 rf_token，管理员在 /admin 登录后打开 /h5，
// H5 会读到管理员的 token —— 页面能进，但"我的预约"永远是空的（userId 是管理员）。
// 这是 P4→P5 最容易出的事故（ARCH-P5 §3.2），必须在第一行就隔离。
// =============================================================================

export const TOKEN_KEY = 'rf_h5_token' // admin 用 'rf_admin_token'
export const USER_KEY = 'rf_h5_user' // admin 用 'rf_admin_user'

/** P6：上次注册/登录用的学号，登录页自动预填（省得学生再敲一遍） */
export const LAST_USERNAME_KEY = 'rf_h5_last_username'
