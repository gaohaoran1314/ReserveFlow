// src/api/resource.js —— 资源域（学生端只读）
import http from './http'

/** 不传 status：维护中的资源也展示但置灰（§5.2） */
export const listResources = (params) => http.get('/resources', { params })
export const getResource = (id) => http.get(`/resources/${id}`)
export const getMeta = () => http.get('/resources/meta')
export const getSlots = (id, date) => http.get(`/resources/${id}/slots`, { params: { date } })
