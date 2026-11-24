<template>
  <div class="rf-page-header">
    <div>
      <h2>{{ title }}</h2>
      <div v-if="desc" class="rf-desc">{{ desc }}</div>
    </div>
    <div class="rf-page-header-actions">
      <slot name="actions" />
    </div>
  </div>
</template>

<script setup>
// 页头：标题 + 副标题 + 右侧操作区（被 6 个页面复用）
defineProps({
  title: { type: String, required: true },
  desc: { type: String, default: '' },
})
</script>

<style scoped>
.rf-page-header-actions {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
</style>
