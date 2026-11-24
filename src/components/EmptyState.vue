<template>
  <div class="rf-empty-block">
    <el-empty :description="description">
      <template #image>
        <el-icon :size="46" style="color: #c0c4cc">
          <component :is="icon" />
        </el-icon>
      </template>
      <slot />
    </el-empty>
    <div
      v-if="desc"
      class="rf-muted"
      style="margin-top: -12px; text-align: center; line-height: 1.8; padding: 0 24px"
    >
      {{ desc }}
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'

// 空态 / 能力缺失提示（列表无数据，或某个后端接口尚未提供时用）
const props = defineProps({
  /** 主文案（不传则显示「暂无数据」） */
  title: { type: String, default: '' },
  /** 补充说明（可换行，用小号灰字展示） */
  desc: { type: String, default: '' },
  /** Element Plus 图标组件名 */
  icon: { type: String, default: 'InfoFilled' },
})

const description = computed(() => props.title || '暂无数据')
</script>
