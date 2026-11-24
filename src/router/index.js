// =============================================================================
// src/router/index.js  —  路由表（Hash 模式 + 全部懒加载）
// 对应 ARCH-P4 §2.1 / D-P4-1
//
// 为什么用 Hash：后端只能挂静态目录，没有 history fallback；
// Hash 模式下深链、刷新、`file://` 双击都不会 404。
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
    path: '/',
    component: () => import('../layouts/AdminLayout.vue'),
    redirect: '/dashboard',
    children: [
      {
        path: 'dashboard',
        name: 'dashboard',
        component: () => import('../views/DashboardView.vue'),
        meta: { title: '概览', icon: 'Odometer' },
      },
      {
        path: 'resources',
        name: 'resources',
        component: () => import('../views/ResourceListView.vue'),
        meta: { title: '资源管理', icon: 'Box' },
      },
      {
        path: 'reservations',
        name: 'reservations',
        component: () => import('../views/ReservationListView.vue'),
        meta: { title: '预约管理', icon: 'Tickets' },
      },
      {
        path: 'rules',
        name: 'rules',
        component: () => import('../views/RuleListView.vue'),
        meta: { title: '规则管理', icon: 'SetUp' },
      },
      {
        path: 'users',
        name: 'users',
        component: () => import('../views/UserCreditView.vue'),
        meta: { title: '用户与信用', icon: 'User' },
      },
      {
        path: 'system',
        name: 'system',
        component: () => import('../views/SystemView.vue'),
        meta: { title: '系统运维', icon: 'Monitor' },
      },
    ],
  },
  {
    path: '/:pathMatch(.*)*',
    name: 'notFound',
    component: () => import('../views/NotFoundView.vue'),
    meta: { public: true, title: '页面不存在' },
  },
]

const router = createRouter({
  history: createWebHashHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 }),
})

// 守卫单独成文件，便于单测与阅读
setupGuard(router)

// 同步浏览器标签标题
router.afterEach((to) => {
  const t = to.meta?.title
  document.title = t ? `${t} · ReserveFlow 管理后台` : 'ReserveFlow 管理后台'
})

export default router
