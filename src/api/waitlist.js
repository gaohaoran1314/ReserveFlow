// src/api/waitlist.js —— 候补域（P4 管理后台不主用，保留以便后续批次）
import http from './http'

export const joinWaitlist = (resourceId, body) => http.post(`/resources/${resourceId}/waitlist`, body)
export const listWaitlist = (params) => http.get('/waitlist', { params })
export const leaveWaitlist = (id) => http.delete(`/waitlist/${id}`)
export const confirmWaitlist = (id) => http.post(`/waitlist/${id}/confirm`)
