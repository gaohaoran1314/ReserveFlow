// src/api/auth.js —— 登录 / 登出 / 当前用户
import http from './http'

export const login = (body) => http.post('/auth/login', body)
/** P6：学生自助注册（公开接口）。成功返回 {id,userId,username,status,statusLabel}，**不含 token** */
export const register = (body, config) => http.post('/auth/register', body, config)
export const logout = () => http.post('/auth/logout')
export const getMe = () => http.get('/auth/me')
