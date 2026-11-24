// src/api/system.js —— 运维与用户列表
import http from './http'

/** 探针：**永远 code=0**，服务是否健康要读 data.mysql.status / data.redis.status（D-P4-4） */
export const getHealth = () => http.get('/health/deps')
export const runMaintenance = () => http.post('/admin/jobs/maintenance')
/** P4 T00 新增：用户列表（ADMIN） */
export const listUsers = (params) => http.get('/admin/users', { params })

// ---- P6：账号审核（ADMIN）----
// 语义是 CAS：只有 PENDING 的账号能通过/拒绝，否则 2007；
// 对「已经是目标状态」的账号重复点会**幂等成功**（后端刻意设计，不报错）。
export const approveUser = (id) => http.post(`/admin/users/${id}/approve`, {})
export const rejectUser = (id, reason) => http.post(`/admin/users/${id}/reject`, { reason: reason || '' })
