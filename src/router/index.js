// =============================================================================
// src/router/index.js  —  8 条路由（Hash 模式 + 懒加载；P6 新增 /register）
// 对应 ARCH-P5 §2.1
//
// Hash 模式的理由与 P4 相同：后端只能挂静态目录、无 history fallback，
// hash 是唯一零改动方案（深链刷新不会 404）。
// =============================================================================
import { createRouter, createWebHashHistory } from 'vue-router'

import { setupGuard } from './guard'

const routes = [
  {
    path: '/login',
    name: 'login',
    component: () => import('../views/LoginView.vue'),
    meta: { public: true, title: '登录' },
  },
  {
    // P6：注册页。必须 public —— 未登录的人才能进来（否则会被守卫弹回 /login）
    // 不设 tabbar：注册页是"门外"页面，不该出现已登录才有的底部导航
    path: '/register',
    name: 'register',
    component: () => import('../views/RegisterView.vue'),
    meta: { public: true, title: '注册' },
  },
  {
    path: '/home',
    name: 'home',
    component: () => import('../views/HomeView.vue'),
    meta: { title: '首页', tabbar: true },
  },
  {
    path: '/resource/:id',
    name: 'resource',
    component: () => import('../views/ResourceDetailView.vue'),
    meta: { title: '资源详情' },
  },
  {
    path: '/reservations',
    name: 'reservations',
    component: () => import('../views/ReservationsView.vue'),
    meta: { title: '我的预约', tabbar: true },
  },
  {
    path: '/waitlist',
    name: 'waitlist',
    component: () => import('../views/WaitlistView.vue'),
    meta: { title: '候补', tabbar: true },
  },
  {
    path: '/credit',
    name: 'credit',
    component: () => import('../views/CreditView.vue'),
    meta: { title: '我的信用' },
  },
  {
    path: '/profile',
    name: 'profile',
    component: () => import('../views/ProfileView.vue'),
    meta: { title: '我的', tabbar: true },
  },
  // H5 不做 404 页：未知路径直接回首页
  {
    path: '/:pathMatch(.*)*',
    redirect: '/home',
  },
]

const router = createRouter({
  history: createWebHashHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 }),
})

setupGuard(router)

router.afterEach((to) => {
  const t = to.meta?.title
  document.title = t ? `${t} · ReserveFlow` : 'ReserveFlow 预约'
})

export default router
