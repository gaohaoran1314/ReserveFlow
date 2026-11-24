// src/api/reservation.js —— 预约域（学生端可用动作只有 取消/取用/归还）
import http from './http'

/** 后端对非管理员会强制按本人过滤，前端不传 userId */
export const listReservations = (params) => http.get('/reservations', { params })
export const getReservation = (id) => http.get(`/reservations/${id}`)
export const createReservation = (body) => http.post('/reservations', body)
export const cancelReservation = (id, body = {}) => http.post(`/reservations/${id}/cancel`, body)
export const pickupReservation = (id, body = {}) => http.post(`/reservations/${id}/pickup`, body)
export const returnReservation = (id, body = {}) => http.post(`/reservations/${id}/return`, body)
export const listDeposits = (id) => http.get(`/reservations/${id}/deposits`)
export const getTimeline = (id) => http.get(`/reservations/${id}/timeline`)
