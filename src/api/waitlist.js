// src/api/waitlist.js —— 候补域
import http from './http'

export const joinWaitlist = (resourceId, body) => http.post(`/resources/${resourceId}/waitlist`, body)
export const listWaitlist = (params) => http.get('/waitlist', { params })
export const leaveWaitlist = (id) => http.delete(`/waitlist/${id}`)
export const confirmOffer = (id) => http.post(`/waitlist/${id}/confirm`)
