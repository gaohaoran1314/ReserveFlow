<template>
  <div class="rf-login-wrap">
    <div class="rf-login-card">
      <div class="rf-login-brand">
        <el-icon :size="30"><Calendar /></el-icon>
        <div>
          <h1>ReserveFlow 管理后台</h1>
          <p>预约资源管理系统 · 管理员入口</p>
        </div>
      </div>

      <el-form
        ref="formRef"
        :model="form"
        :rules="rules"
        label-position="top"
        size="large"
        @submit.prevent="onSubmit"
      >
        <el-form-item label="账号" prop="username">
          <el-input
            v-model="form.username"
            placeholder="请输入管理员账号"
            autocomplete="username"
            clearable
            @keyup.enter="onSubmit"
          >
            <template #prefix><el-icon><User /></el-icon></template>
          </el-input>
        </el-form-item>

        <el-form-item label="密码" prop="password">
          <el-input
            v-model="form.password"
            type="password"
            placeholder="请输入密码"
            autocomplete="current-password"
            show-password
            @keyup.enter="onSubmit"
          >
            <template #prefix><el-icon><Lock /></el-icon></template>
          </el-input>
        </el-form-item>

        <el-alert
          v-if="errorMsg"
          :title="errorMsg"
          type="error"
          :closable="false"
          show-icon
          style="margin-bottom: 12px"
        />

        <el-button
          type="primary"
          size="large"
          style="width: 100%"
          :loading="loading"
          @click="onSubmit"
        >
          登录
        </el-button>
      </el-form>

      <div class="rf-login-tip">
        演示账号：<span class="rf-mono">admin / Admin@123456</span>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted, reactive, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'

import { useAuthStore } from '../stores/auth'
import { LOCAL_TIP } from '../constants/errorCode'

const route = useRoute()
const router = useRouter()
const auth = useAuthStore()

const formRef = ref(null)
const loading = ref(false)
const errorMsg = ref('')

const form = reactive({
  username: '',
  password: '',
})

const rules = {
  username: [{ required: true, message: '请输入账号', trigger: 'blur' }],
  password: [{ required: true, message: '请输入密码', trigger: 'blur' }],
}

onMounted(() => {
  // 已登录（含刷新后仍有 token）→ 直接进后台，不再显示登录页
  if (auth.token) {
    router.replace({ name: 'dashboard' })
  }
})

async function onSubmit() {
  if (!formRef.value) return
  try {
    await formRef.value.validate()
  } catch {
    return // 表单校验未通过，Element Plus 已在字段下提示
  }

  loading.value = true
  errorMsg.value = ''
  try {
    const data = await auth.login(form.username.trim(), form.password)

    // 非管理员：后端允许登录，但后台禁止进入（提示明确，不说"密码错误"）
    if (data?.user?.role !== 'ADMIN') {
      auth.clear()
      errorMsg.value = LOCAL_TIP.notAdmin
      ElMessage.error(LOCAL_TIP.notAdmin)
      return
    }

    ElMessage.success(`欢迎回来，${data?.user?.username || '管理员'}`)

    // 回跳：优先 redirect（可带 query，如 /reservations?id=239），否则去概览
    const redirect = route.query.redirect
    if (typeof redirect === 'string' && redirect.startsWith('/')) {
      router.replace(redirect)
    } else {
      router.replace({ name: 'dashboard' })
    }
  } catch (e) {
    // 后端 msg 已是可直接展示的中文（如「账号或密码错误」2000）
    errorMsg.value = e?.msg || '登录失败，请稍后重试'
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.rf-login-wrap {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  background: linear-gradient(135deg, #1f2d3d 0%, #2c3e50 55%, #409eff 160%);
}

.rf-login-card {
  width: 400px;
  background: #fff;
  border-radius: 8px;
  padding: 32px 32px 24px;
  box-shadow: 0 12px 40px rgba(0, 0, 0, 0.24);
}

.rf-login-brand {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 24px;
  color: #1f2d3d;
}

.rf-login-brand h1 {
  margin: 0;
  font-size: 20px;
  font-weight: 600;
}

.rf-login-brand p {
  margin: 4px 0 0;
  font-size: 12px;
  color: #909399;
}

.rf-login-tip {
  margin-top: 16px;
  text-align: center;
  font-size: 12px;
  color: #909399;
}
</style>
