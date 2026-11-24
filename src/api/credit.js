// src/api/credit.js —— 信用域
import http from './http'

export const getMyCredit = () => http.get('/credit/me')
export const getMyCreditLogs = (params) => http.get('/credit/me/logs', { params })
export const getUserCredit = (userId) => http.get(`/admin/credit/${userId}`)
export const adjustCredit = (userId, body) => http.post(`/admin/credit/${userId}/adjust`, body)
