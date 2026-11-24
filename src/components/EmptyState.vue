<template>
  <div class="rf-empty">
    <div class="rf-empty-icon">{{ icon }}</div>
    <div class="rf-empty-text">{{ text }}</div>
    <slot name="action">
      <van-button v-if="actionText" size="small" type="primary" plain @click="$emit('action')">
        {{ actionText }}
      </van-button>
    </slot>
  </div>
</template>

<script setup>
/**
 * 空态（ARCH-P5 §4）
 * 用 emoji 当图标：不引入图标库，离线零成本。
 */
defineProps({
  text: { type: String, default: '暂无数据' },
  icon: { type: String, default: '📭' },
  actionText: { type: String, default: '' },
})

defineEmits(['action'])
</script>
