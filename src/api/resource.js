// src/api/resource.js —— 资源域
import http from './http'

export const listResources = (params) => http.get('/resources', { params })
export const getResource = (id) => http.get(`/resources/${id}`)
export const createResource = (body) => http.post('/resources', body)
export const updateResource = (id, body) => http.put(`/resources/${id}`, body)
export const patchStatus = (id, status) => http.patch(`/resources/${id}/status`, { status })
export const removeResource = (id) => http.delete(`/resources/${id}`)
export const getMeta = () => http.get('/resources/meta')
export const getSlots = (id, date) => http.get(`/resources/${id}/slots`, { params: { date } })
