<template>
  <div class="rf-login">
    <div class="rf-login-hd">
      <div class="rf-login-logo">📅</div>
      <h1>ReserveFlow 预约</h1>
      <p>实验室仪器 · 会议室 · 储物柜</p>
    </div>

    <van-form class="rf-login-form" @submit="onSubmit">
      <van-cell-group inset>
        <van-field
          v-model="form.username"
          name="username"
          label="学号"
          placeholder="请输入学号 / 账号"
          autocomplete="username"
          autocapitalize="off"
          autocorrect="off"
          clearable
          :rules="[{ required: true, message: '请输入学号' }]"
        />
        <van-field
          v-model="form.password"
          type="password"
          name="password"
          label="密码"
          placeholder="请输入密码"
          autocomplete="current-password"
          :rules="[{ required: true, message: '请输入密码' }]"
        />
      </van-cell-group>

      <div style="padding: 16px">
        <van-button round block type="primary" native-type="submit" :loading="loading">
          登录
        </van-button>
      </div>
    </van-form>

    <!-- P6：注册入口 -->
    <div class="rf-login-register">
      还没有账号？<a href="javascript:void(0)" @click="goRegister">立即注册</a>
    </div>

    <div class="rf-login-tip">
      演示账号：<span class="rf-mono">student01 / User@123456</span>
    </div>
  </div>
</template>

<script setup>
import { onMounted, reactive, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { showFailToast, showSuccessToast } from 'vant'

import { useAuthStore } from '../stores/auth'
import { LOCAL_TIP } from '../constants/errorCode'
import { LAST_USERNAME_KEY } from '../constants/storage'

const route = useRoute()
const router = useRouter()
const auth = useAuthStore()

const loading = ref(false)
const form = reactive({ username: '', password: '' })

onMounted(() => {
  // 已登录（含刷新后仍有 token）→ 直接进首页
  if (auth.token) {
    router.replace({ name: 'home' })
    return
  }
  // P6：注册成功后会把学号留在本地，这里自动预填，学生只需输密码
  const last = localStorage.getItem(LAST_USERNAME_KEY)
  if (last) form.username = last
})

function goRegister() {
  router.push({ name: 'register' })
}

async function onSubmit() {
  loading.value = true
  try {
    const data = await auth.login(form.username.trim(), form.password)

    // 禁用账号：后端若放行登录，这里也一律拒绝（守卫同样会拦）
    if (data?.user?.status === 'DISABLED') {
      auth.clear()
      showFailToast(LOCAL_TIP.disabled)
      return
    }

    showSuccessToast(`欢迎回来，${data?.user?.realName || data?.user?.username || ''}`)

    // 黑名单**不在这里拦**（主理人决策 3）：登录成功，首页用 notice-bar 提示
    const redirect = route.query.redirect
    if (typeof redirect === 'string' && redirect.startsWith('/')) {
      router.replace(redirect)
    } else {
      router.replace({ name: 'home' })
    }
  } catch {
    // 后端 msg 已是中文（如「账号或密码错误」2000），拦截器已 toast
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.rf-login {
  min-height: 100vh;
  background: linear-gradient(180deg, #1989fa 0%, #4aa8ff 32%, var(--rf-bg) 32%);
  padding-top: calc(48px + var(--safe-top));
}

.rf-login-hd {
  text-align: center;
  color: #fff;
  margin-bottom: 24px;
}

.rf-login-logo {
  font-size: 44px;
  line-height: 1;
}

.rf-login-hd h1 {
  margin: 10px 0 4px;
  font-size: 21px;
  font-weight: 600;
}

.rf-login-hd p {
  margin: 0;
  font-size: 12px;
  opacity: 0.85;
}

.rf-login-form {
  background: #fff;
  border-radius: 12px;
  margin: 0 16px;
  padding: 8px 0 4px;
  box-shadow: 0 6px 18px rgba(25, 137, 250, 0.12);
}

.rf-login-register {
  margin-top: 16px;
  text-align: center;
  font-size: 13px;
  color: var(--rf-text-2);
}

.rf-login-register a {
  color: var(--rf-primary);
  text-decoration: none;
}

.rf-login-tip {
  margin-top: 10px;
  text-align: center;
  font-size: 12px;
  color: var(--rf-text-2);
}
</style>
