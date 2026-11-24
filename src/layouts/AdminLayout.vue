<template>
  <div class="rf-app">
    <div class="rf-layout">
      <!-- ---------------- 侧边栏 ---------------- -->
      <aside class="rf-sidebar" :class="{ 'is-collapsed': ui.sidebarCollapsed }">
        <div class="rf-brand">
          <el-icon><Calendar /></el-icon>
          <span v-show="!ui.sidebarCollapsed">ReserveFlow 管理后台</span>
        </div>
        <el-menu
          :default-active="activeMenu"
          :collapse="ui.sidebarCollapsed"
          :collapse-transition="false"
          background-color="#1f2d3d"
          text-color="#bfcbd9"
          active-text-color="#ffffff"
          router
          style="border-right: none; flex: 1"
        >
          <el-menu-item v-for="m in menus" :key="m.name" :index="m.path">
            <el-icon><component :is="m.icon" /></el-icon>
            <template #title>{{ m.title }}</template>
          </el-menu-item>
        </el-menu>
      </aside>

      <!-- ---------------- 主区 ---------------- -->
      <div class="rf-main">
        <header class="rf-header">
          <div class="rf-header-left">
            <el-button text @click="ui.toggleSidebar()">
              <el-icon><component :is="ui.sidebarCollapsed ? 'Expand' : 'Fold'" /></el-icon>
            </el-button>
            <span class="rf-muted">{{ currentTitle }}</span>
          </div>

          <div class="rf-header-right">
            <el-tag v-if="auth.role" size="small" :type="auth.isAdmin ? 'danger' : 'info'">
              {{ auth.isAdmin ? '管理员' : '普通用户' }}
            </el-tag>
            <el-dropdown trigger="click" @command="onCommand">
              <span style="cursor: pointer; display: inline-flex; align-items: center; gap: 6px">
                <el-icon><UserFilled /></el-icon>
                <span>{{ auth.displayName || '未登录' }}</span>
                <el-icon><ArrowDown /></el-icon>
              </span>
              <template #dropdown>
                <el-dropdown-menu>
                  <el-dropdown-item command="refresh">刷新我的信息</el-dropdown-item>
                  <el-dropdown-item command="logout" divided>退出登录</el-dropdown-item>
                </el-dropdown-menu>
              </template>
            </el-dropdown>
          </div>
        </header>

        <main class="rf-content">
          <router-view />
        </main>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'

import { useAuthStore } from '../stores/auth'
import { useUiStore } from '../stores/ui'

const route = useRoute()
const router = useRouter()
const auth = useAuthStore()
const ui = useUiStore()

/** 侧边栏菜单（与 router/index.js 的 6 条业务路由一一对应） */
const menus = [
  { name: 'dashboard', path: '/dashboard', title: '概览', icon: 'Odometer' },
  { name: 'resources', path: '/resources', title: '资源管理', icon: 'Box' },
  { name: 'reservations', path: '/reservations', title: '预约管理', icon: 'Tickets' },
  { name: 'rules', path: '/rules', title: '规则管理', icon: 'SetUp' },
  { name: 'users', path: '/users', title: '用户与信用', icon: 'User' },
  { name: 'system', path: '/system', title: '系统运维', icon: 'Monitor' },
]

// 详情抽屉用 query（?id=239）时，菜单仍高亮父级路由
const activeMenu = computed(() => route.path)
const currentTitle = computed(() => route.meta?.title || '')

async function onCommand(cmd) {
  if (cmd === 'refresh') {
    try {
      await auth.fetchMe()
      ElMessage.success('已刷新当前账号信息')
    } catch {
      // 失败提示已由拦截器给出
    }
    return
  }
  if (cmd === 'logout') {
    try {
      await ElMessageBox.confirm('确认退出登录？', '退出登录', {
        confirmButtonText: '退出',
        cancelButtonText: '取消',
        type: 'warning',
      })
    } catch {
      return // 用户取消
    }
    await auth.logout()
    ElMessage.success('已退出登录')
    router.replace({ name: 'login' })
  }
}
</script>
