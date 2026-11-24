<template>
  <div class="rf-h5">
    <router-view v-slot="{ Component }">
      <component :is="Component" />
    </router-view>

    <!-- 只在 4 个主路由显示（详情页/信用页隐藏），fixed + placeholder 防遮挡 -->
    <van-tabbar
      v-if="showTabbar"
      route
      fixed
      placeholder
      safe-area-inset-bottom
      active-color="#1989fa"
      inactive-color="#7d7e80"
    >
      <van-tabbar-item replace to="/home" icon="home-o">首页</van-tabbar-item>
      <van-tabbar-item replace to="/reservations" icon="orders-o">我的预约</van-tabbar-item>
      <van-tabbar-item replace to="/waitlist" icon="underway-o">候补</van-tabbar-item>
      <van-tabbar-item replace to="/profile" icon="user-o">我的</van-tabbar-item>
    </van-tabbar>
  </div>
</template>

<script setup>
import { computed, onMounted } from 'vue'
import { useRoute } from 'vue-router'

import { useAuthStore } from './stores/auth'

const route = useRoute()
const auth = useAuthStore()

/** TabBar 显示条件由路由 meta 决定（ARCH-P5 §2.3） */
const showTabbar = computed(() => route.meta?.tabbar === true)

/**
 * 冷启动刷新一次用户信息。
 *
 * `user` 是登录时写进 localStorage 的快照，可能是几天前的：
 * 信用分、黑名单状态都会过期（管理员调分/拉黑后，不重新登录就永远看到旧值）。
 * 这里在启动时拉一次 /auth/me 合并进 store —— 一次请求，不做轮询；
 * **失败静默**，因为登录态的快照足以让页面正常显示。
 */
onMounted(() => {
  if (!auth.restored) auth.restore()
  if (auth.token) {
    auth.fetchMe().catch(() => {})
  }
})
</script>
