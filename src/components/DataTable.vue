<template>
  <div>
    <el-table
      v-loading="loading"
      :data="rows"
      border
      stripe
      size="default"
      :empty-text="emptyText"
      style="width: 100%"
    >
      <el-table-column
        v-for="col in columns"
        :key="col.prop || col.label"
        :prop="col.prop"
        :label="col.label"
        :width="col.width"
        :min-width="col.minWidth"
        :align="col.align || 'left'"
        :fixed="col.fixed"
        :show-overflow-tooltip="col.overflow !== false"
      >
        <template #default="scope">
          <!-- 具名插槽：col-<prop>，页面自定义渲染（状态标签/操作列/多行文本等） -->
          <slot :name="`col-${col.prop}`" :row="scope.row" :index="scope.$index">
            {{ display(scope.row, col) }}
          </slot>
        </template>
      </el-table-column>

      <template #empty>
        <slot name="empty">
          <span class="rf-muted">{{ emptyText }}</span>
        </slot>
      </template>
    </el-table>

    <div v-if="showPager" class="rf-pager">
      <el-pagination
        :current-page="page"
        :page-size="size"
        :total="total"
        :page-sizes="[10, 20, 50, 100]"
        layout="total, sizes, prev, pager, next, jumper"
        background
        @current-change="(p) => emit('change-page', p)"
        @size-change="(s) => emit('change-size', s)"
      />
    </div>
  </div>
</template>

<script setup>
/**
 * 表格 + 分页 + loading + 空态一体化封装（被 5 个列表页复用）
 *
 * 用法：
 *   <DataTable :columns="cols" :rows="list" :total="total" :page="page" :size="size"
 *              :loading="loading" @change-page="onPage" @change-size="onSize">
 *     <template #col-status="{ row }"><StatusTag :value="row.status" /></template>
 *     <template #col-actions="{ row }"><el-button @click="open(row)">详情</el-button></template>
 *   </DataTable>
 */
const props = defineProps({
  /** [{ prop, label, width?, minWidth?, align?, fixed?, overflow? }] */
  columns: { type: Array, required: true },
  rows: { type: Array, default: () => [] },
  total: { type: Number, default: 0 },
  page: { type: Number, default: 1 },
  size: { type: Number, default: 10 },
  loading: { type: Boolean, default: false },
  emptyText: { type: String, default: '暂无数据' },
  /** 规则/候补这类"一次返回全部"的列表不显示分页 */
  showPager: { type: Boolean, default: true },
})

const emit = defineEmits(['change-page', 'change-size'])

/**
 * 单元格默认取值：支持 `a.b` 形式的浅嵌套
 * @param {object} row
 * @param {object} col
 */
function display(row, col) {
  if (!col.prop) return ''
  const parts = String(col.prop).split('.')
  let v = row
  for (const p of parts) {
    v = v?.[p]
    if (v === undefined || v === null) return '—'
  }
  if (typeof v === 'boolean') return v ? '是' : '否'
  return v
}
</script>

<style scoped>
.rf-pager {
  display: flex;
  justify-content: flex-end;
  padding-top: 12px;
}
</style>
