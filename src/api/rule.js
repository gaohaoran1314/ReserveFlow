// src/api/rule.js —— 规则管理域（全部 ADMIN）
import http from './http'

export const listRules = (params) => http.get('/admin/rules', { params })
export const getRule = (id) => http.get(`/admin/rules/${id}`)
export const createRule = (body) => http.post('/admin/rules', body)
export const updateRule = (id, body) => http.put(`/admin/rules/${id}`, body)
export const setRuleEnabled = (id, enabled) => http.patch(`/admin/rules/${id}/enabled`, { enabled })
export const setRulePriority = (id, priority) => http.patch(`/admin/rules/${id}/priority`, { priority })
export const removeRule = (id) => http.delete(`/admin/rules/${id}`)
export const validateRule = (body) => http.post('/admin/rules/validate', body)
