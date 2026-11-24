<template>
  <div>
    <van-nav-bar title="我的" />

    <!-- 下拉刷新：重新拉 /auth/me，避免显示登录那一刻的旧信用分/旧状态 -->
    <van-pull-refresh v-model="refreshing" @refresh="onRefresh">
      <div class="rf-page">
      <!-- 用户信息 -->
      <div class="rf-card rf-profile-hd">
        <div class="rf-avatar">{{ avatarChar }}</div>
        <div class="rf-grow" style="min-width: 0">
          <div class="rf-profile-name">{{ auth.displayName || '未登录' }}</div>
          <div class="rf-card-sub">
            {{ auth.username }}
            <van-tag
              size="medium"
              :type="auth.role === 'ADMIN' ? 'danger' : 'primary'"
              plain
              style="margin-left: 6px"
            >
              {{ auth.role === 'ADMIN' ? '管理员' : '学生' }}
            </van-tag>
          </div>
        </div>
      </div>

      <!-- 信用概览（点击进信用页） -->
      <div class="rf-card rf-credit-overview" @click="router.push({ name: 'credit' })">
        <div class="rf-row-between">
          <div>
            <div class="rf-card-sub">我的信用分</div>
            <div class="rf-score" :style="{ color: creditColor(auth.creditScore) }">
              {{ auth.creditScore ?? '—' }}
            </div>
          </div>
          <div style="text-align: right">
            <StatusTag type="user" :value="auth.status" />
            <div class="rf-card-sub" style="margin-top: 4px">查看明细 ›</div>
          </div>
        </div>
      </div>

      <van-cell-group inset>
        <van-cell title="我的预约" icon="orders-o" is-link @click="router.push({ name: 'reservations' })" />
        <van-cell title="我的候补" icon="underway-o" is-link @click="router.push({ name: 'waitlist' })" />
        <van-cell title="我的信用" icon="star-o" is-link @click="router.push({ name: 'credit' })" />
      </van-cell-group>

      <van-cell-group inset style="margin-top: 12px">
        <van-cell title="退出登录" icon="revoke" is-link @click="onLogout" />
      </van-cell-group>

      <div class="rf-muted" style="text-align: center; font-size: 12px; margin-top: 20px">
        ReserveFlow 学生端 · v0.5 · 下拉可刷新
      </div>
      </div>
    </van-pull-refresh>
  </div>
</template>

<script setup>
import { computed, onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { showConfirmDialog, showFailToast, showSuccessToast } from 'vant'

import StatusTag from '../components/StatusTag.vue'
import { useAuthStore } from '../stores/auth'
import { creditColor } from '../utils/format'

const router = useRouter()
const auth = useAuthStore()

/** 下拉刷新的 loading（van-pull-refresh 的 v-model） */
const refreshing = ref(false)

/**
 * 重新拉一次 /auth/me 并合并进 store。
 * 为什么必须做：`auth.user` 是**登录那一刻的快照**（存 localStorage），
 * 管理员事后调分 / 拉黑，不重新拉就永远显示旧值 —— 「我的」页曾长期显示陈旧信用分，
 * 而「我的信用」页每次进页面都拉 /credit/me 所以是对的，两个页面数字打架。
 *
 * 这里用 `refreshIfStale(0)` —— ttl=0 表示**必刷、不节流**：
 * 用户点进「我的」就是想立刻看到最新数字，不该被 15 秒节流挡住。
 * 内部已做并发去重与失败静默，页面不用再包 try/catch。
 */
async function refreshUser() {
  await auth.refreshIfStale(0)
}

async function onRefresh() {
  refreshing.value = true
  try {
    await refreshUser()
  } finally {
    refreshing.value = false
  }
}

onMounted(refreshUser)

/** 头像位：取名字首字（中文取第一个字，英文取首字母大写） */
const avatarChar = computed(() => {
  const n = auth.displayName || auth.username || '?'
  return String(n).slice(0, 1).toUpperCase()
})

async function onLogout() {
  try {
    await showConfirmDialog({
      title: '退出登录',
      message: '确认退出当前账号？',
      confirmButtonText: '退出',
      confirmButtonColor: '#ee0a24',
    })
  } catch {
    return
  }

  try {
    await auth.logout()
    showSuccessToast('已退出登录')
    router.replace({ name: 'login' })
  } catch {
    showFailToast('退出失败，请重试')
  }
}
</script>

<style scoped>
.rf-profile-hd {
  display: flex;
  align-items: center;
  gap: 12px;
}

.rf-avatar {
  width: 52px;
  height: 52px;
  flex: 0 0 52px;
  border-radius: 50%;
  background: linear-gradient(135deg, #1989fa, #4aa8ff);
  color: #fff;
  font-size: 22px;
  font-weight: 600;
  display: flex;
  align-items: center;
  justify-content: center;
}

.rf-profile-name {
  font-size: 17px;
  font-weight: 600;
  line-height: 1.4;
}

.rf-credit-overview {
  cursor: pointer;
}

.rf-score {
  font-size: 30px;
  font-weight: 700;
  line-height: 1.2;
  font-variant-numeric: tabular-nums;
}
</style>
