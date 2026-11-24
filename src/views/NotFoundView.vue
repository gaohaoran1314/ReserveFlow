<template>
  <div class="rf-notfound">
    <el-result icon="warning" title="404" sub-title="页面不存在，或该地址已被移除">
      <template #extra>
        <el-button type="primary" @click="goHome">返回概览</el-button>
      </template>
    </el-result>
  </div>
</template>

<script setup>
import { useRouter } from 'vue-router'

const router = useRouter()

function goHome() {
  router.replace({ name: 'dashboard' })
}
</script>

<style scoped>
.rf-notfound {
  padding-top: 64px;
}
</style>
