// src/api/credit.js —— 信用域（本人视角）
import http from './http'

export const getMyCredit = () => http.get('/credit/me')
export const listMyCreditLogs = (params) => http.get('/credit/me/logs', { params })
