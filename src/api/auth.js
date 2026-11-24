// src/api/auth.js —— 登录 / 登出 / 当前用户
import http from './http'

export const login = (body) => http.post('/auth/login', body)
export const logout = () => http.post('/auth/logout')
export const getMe = () => http.get('/auth/me')
