<template>
  <div class="rf-filter-bar">
    <el-form :inline="true" :model="{}" @submit.prevent="emit('search')">
      <slot />
      <el-form-item>
        <el-button type="primary" :loading="loading" @click="emit('search')">查询</el-button>
        <el-button @click="emit('reset')">重置</el-button>
      </el-form-item>
    </el-form>
  </div>
</template>

<script setup>
/**
 * 筛选条容器：把「查询 / 重置」统一到右侧，页面只写自己的字段。
 * 回车也能触发查询（Form 的 submit.prevent）。
 */
defineProps({
  loading: { type: Boolean, default: false },
})

const emit = defineEmits(['search', 'reset'])
</script>

<style scoped>
.rf-filter-bar {
  margin-bottom: 4px;
}

.rf-filter-bar :deep(.el-form-item) {
  margin-bottom: 12px;
}
</style>
