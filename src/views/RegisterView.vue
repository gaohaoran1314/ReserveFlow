<template>
  <div class="rf-register">
    <van-nav-bar title="注册账号" left-arrow @click-left="goLogin" />

    <div class="rf-register-hd">
      <div class="rf-register-logo">📝</div>
      <p>注册后需管理员审核，通过后即可用学号登录</p>
    </div>

    <van-form class="rf-register-form" @submit="onSubmit">
      <van-cell-group inset>
        <van-field
          v-model="form.username"
          name="username"
          label="学号"
          placeholder="3–32 位字母、数字或下划线"
          autocomplete="username"
          autocapitalize="off"
          autocorrect="off"
          clearable
          :rules="usernameRules"
        />
        <van-field
          v-model="form.realName"
          name="realName"
          label="姓名"
          placeholder="请输入真实姓名"
          maxlength="64"
          clearable
          :rules="realNameRules"
        />
        <van-field
          v-model="form.password"
          type="password"
          name="password"
          label="密码"
          placeholder="8–64 位"
          autocomplete="new-password"
          :rules="passwordRules"
        />
        <van-field
          v-model="form.password2"
          type="password"
          name="password2"
          label="确认密码"
          placeholder="请再次输入密码"
          autocomplete="new-password"
          :rules="password2Rules"
        />
      </van-cell-group>

      <div style="padding: 16px">
        <van-button round block type="primary" native-type="submit" :loading="loading">
          注册
        </van-button>
      </div>
    </van-form>

    <div class="rf-register-tip">
      已有账号？<a href="javascript:void(0)" @click="goLogin">返回登录</a>
    </div>
  </div>
</template>

<script setup>
import { reactive, ref } from 'vue'
import { useRouter } from 'vue-router'
import { showDialog, showFailToast, showToast } from 'vant'

import { register } from '../api/auth'
import { LAST_USERNAME_KEY } from '../constants/storage'

const router = useRouter()

const loading = ref(false)
const form = reactive({ username: '', realName: '', password: '', password2: '' })

// ---- 前端校验（与后端口径一致：学号 3–32 位字母/数字/下划线；密码 8–64）
const usernameRules = [
  { required: true, message: '请输入学号' },
  {
    validator: (v) => /^[A-Za-z0-9_]{3,32}$/.test(String(v || '').trim()),
    message: '学号须为 3–32 位字母、数字或下划线',
  },
]
const realNameRules = [
  { required: true, message: '请输入姓名' },
  {
    validator: (v) => String(v || '').trim().length >= 1 && String(v || '').trim().length <= 64,
    message: '姓名长度为 1–64 个字符',
  },
]
const passwordRules = [
  { required: true, message: '请输入密码' },
  {
    validator: (v) => String(v || '').length >= 8 && String(v || '').length <= 64,
    message: '密码长度为 8–64 位',
  },
]
const password2Rules = [
  { required: true, message: '请再次输入密码' },
  {
    validator: (v) => v === form.password,
    message: '两次输入的密码不一致',
  },
]

function goLogin() {
  router.replace({ name: 'login' })
}

/**
 * 提交注册。
 * 用 `silent` 关掉拦截器的统一 toast —— 因为 2003 / 2004 / 1000 三种情况
 * 需要**完全不同的引导文案**（去登录 / 等审核 / 哪个字段错了），
 * 统一弹后端 msg 会把"请直接登录"这种关键动作提示丢掉。
 */
async function onSubmit() {
  loading.value = true
  try {
    const data = await register(
      {
        username: form.username.trim(),
        realName: form.realName.trim(),
        password: form.password,
      },
      { silent: true }
    )

    // 记住学号，回登录页自动预填
    localStorage.setItem(LAST_USERNAME_KEY, data?.username || form.username.trim())

    await showDialog({
      title: '注册成功',
      message: `学号 ${data?.username || form.username.trim()} 已提交，请等待管理员审核。审核通过后即可用学号登录。`,
      confirmButtonText: '去登录',
      showCancelButton: false,
    })
    router.replace({ name: 'login' })
  } catch (e) {
    handleRegisterError(e)
  } finally {
    loading.value = false
  }
}

/** 按错误码分流（不要一律同一个 toast） */
function handleRegisterError(e) {
  const code = e?.code
  if (code === 2003) {
    // 已被占用：直接引导去登录（最常见的原因是自己早就注册过）
    showDialog({
      title: '该学号已注册',
      message: '该学号已被注册，请直接登录；若忘记密码请联系管理员。',
      confirmButtonText: '去登录',
      showCancelButton: true,
      cancelButtonText: '改一个学号',
    })
      .then(() => router.replace({ name: 'login' }))
      .catch(() => {
        /* 用户选择改学号，留在本页 */
      })
    return
  }
  if (code === 2004) {
    showDialog({
      title: '已提交过',
      message: '该学号已提交注册，正在等待管理员审核，无需重复提交。',
      confirmButtonText: '知道了',
    }).catch(() => {})
    return
  }
  if (code === 1000) {
    // 参数问题：后端 msg 已经指明哪个字段不对，原样展示
    showToast(e?.msg || '参数不合法，请检查后重试')
    return
  }
  showFailToast(e?.msg || '注册失败，请稍后重试')
}
</script>

<style scoped>
.rf-register {
  min-height: 100vh;
  background: var(--rf-bg);
  padding-bottom: 24px;
}

.rf-register-hd {
  text-align: center;
  padding: 20px 16px 12px;
}

.rf-register-logo {
  font-size: 40px;
  line-height: 1;
}

.rf-register-hd p {
  margin: 8px 0 0;
  font-size: 12px;
  color: var(--rf-text-2);
}

.rf-register-form {
  background: #fff;
  border-radius: 12px;
  margin: 0 12px;
  padding: 8px 0 4px;
}

.rf-register-tip {
  margin-top: 18px;
  text-align: center;
  font-size: 13px;
  color: var(--rf-text-2);
}

.rf-register-tip a {
  color: var(--rf-primary);
  text-decoration: none;
}
</style>
