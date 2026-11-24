// src/api/reservation.js —— 预约域（含 P4 T00 新增的 timeline）
import http from './http'

export const listReservations = (params) => http.get('/reservations', { params })
export const getReservation = (id) => http.get(`/reservations/${id}`)
export const createReservation = (body) => http.post('/reservations', body)

// 七个动作（全部 POST，body 可为 {}）
export const cancel = (id, body = {}) => http.post(`/reservations/${id}/cancel`, body)
export const approve = (id, body = {}) => http.post(`/reservations/${id}/approve`, body)
export const reject = (id, body = {}) => http.post(`/reservations/${id}/reject`, body)
export const pickup = (id, body = {}) => http.post(`/reservations/${id}/pickup`, body)
export const returnBack = (id, body = {}) => http.post(`/reservations/${id}/return`, body)
export const accept = (id, body = {}) => http.post(`/reservations/${id}/accept`, body)
export const damage = (id, body = {}) => http.post(`/reservations/${id}/damage`, body)

export const getDeposits = (id) => http.get(`/reservations/${id}/deposits`)
export const getTimeline = (id) => http.get(`/reservations/${id}/timeline`)
