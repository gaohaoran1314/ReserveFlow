<template>
  <div class="rf-json-editor">
    <div class="rf-json-toolbar">
      <span class="rf-muted">{{ label }}</span>
      <div>
        <el-button size="small" @click="format">格式化</el-button>
        <el-button size="small" @click="compact">压缩</el-button>
        <el-button size="small" :type="ok ? 'success' : 'danger'" plain @click="validate(true)">
          {{ ok ? '语法正确' : '校验' }}
        </el-button>
      </div>
    </div>

    <el-input
      :model-value="modelValue"
      type="textarea"
      :rows="rows"
      spellcheck="false"
      class="rf-json-textarea"
      placeholder='例如 {"all":[{"op":"eq","field":"$user.status","value":"BLACKLIST"}]}'
      @update:model-value="(v) => emit('update:modelValue', v)"
      @blur="validate(false)"
    />

    <el-alert
      v-if="error"
      :title="error"
      type="error"
      :closable="false"
      show-icon
      style="margin-top: 6px"
    />
    <div v-else-if="warn" class="rf-dev-warn">{{ warn }}</div>
  </div>
</template>

<script setup>
import { ref, watch } from 'vue'

/**
 * JSON 编辑器（ARCH-P4 §6.5 的三层校验里的第 1、2 层）
 *   第 1 层 本地语法：JSON.parse 捕获异常 → 用 position 反推行列并展示
 *   第 2 层 本地语义：已知 op / action.type 白名单 → 只给黄色警告，不阻断
 *   第 3 层 后端校验：由页面在提交前调 /admin/rules/validate（本组件不负责）
 */
const props = defineProps({
  modelValue: { type: String, default: '' },
  label: { type: String, default: 'JSON' },
  rows: { type: Number, default: 8 },
  /** 是否对 action 类 JSON 做已知 type 提示 */
  actionHint: { type: Boolean, default: false },
})

const emit = defineEmits(['update:modelValue', 'validate'])

const ok = ref(true)
const error = ref('')
const warn = ref('')

const KNOWN_OPS = ['eq', 'ne', 'gt', 'gte', 'lt', 'lte', 'in', 'notIn', 'isNull', 'notNull', 'notEmpty']
const KNOWN_ACTIONS = ['reject', 'deduct_credit', 'deduct_only', 'transition', 'freeze_deposit', 'deduct_deposit']

/** 由字符 position 反推「第几行第几列」 */
function lineColOf(text, position) {
  const before = text.slice(0, Math.max(0, position))
  const lines = before.split('\n')
  return { line: lines.length, column: lines[lines.length - 1].length + 1 }
}

/**
 * 校验
 * @param {boolean} verbose true 时把「校验通过」也提示出来（点按钮时用）
 * @returns {boolean}
 */
function validate(verbose = false) {
  const text = (props.modelValue || '').trim()
  error.value = ''
  warn.value = ''

  if (!text) {
    ok.value = true
    if (verbose) emit('validate', true, '')
    return true
  }

  let parsed = null
  try {
    parsed = JSON.parse(text)
    ok.value = true
  } catch (e) {
    ok.value = false
    // V8 的两种错误格式都覆盖：
    //   "Unexpected token } in JSON at position 42"
    //   "Expected ',' or '}' ... at position 42 (line 3 column 12)"
    const msg = String(e.message || 'JSON 语法错误')
    const m = msg.match(/position\s+(\d+)/i)
    if (m) {
      const { line, column } = lineColOf(text, Number(m[1]))
      error.value = `第 ${line} 行第 ${column} 列：${msg.replace(/\s*in JSON at position.*$/i, '')}`
    } else {
      error.value = msg
    }
    emit('validate', false, error.value)
    return false
  }

  // 第 2 层：语义提示（不阻断）
  if (parsed && typeof parsed === 'object') {
    const hints = []
    const walk = (node) => {
      if (!node || typeof node !== 'object') return
      if (Array.isArray(node)) {
        node.forEach(walk)
        return
      }
      if (typeof node.op === 'string' && !KNOWN_OPS.includes(node.op)) {
        hints.push(`未知 op「${node.op}」`)
      }
      if (typeof node.field === 'string' && !node.field.startsWith('$')) {
        hints.push(`field「${node.field}」应以 $ 开头`)
      }
      Object.values(node).forEach(walk)
    }
    walk(parsed)
    if (props.actionHint && typeof parsed.type === 'string' && !KNOWN_ACTIONS.includes(parsed.type)) {
      hints.push(`未知 action.type「${parsed.type}」`)
    }
    if (hints.length) {
      warn.value = `本地提示（仅供参考，以后端校验为准）：${[...new Set(hints)].join('；')}`
    }
  }

  emit('validate', true, '')
  return true
}

function format() {
  try {
    const parsed = JSON.parse((props.modelValue || '').trim() || '{}')
    emit('update:modelValue', JSON.stringify(parsed, null, 2))
    error.value = ''
    warn.value = ''
    ok.value = true
  } catch {
    validate(true) // 语法错误时展示定位信息
  }
}

function compact() {
  try {
    const parsed = JSON.parse((props.modelValue || '').trim() || '{}')
    emit('update:modelValue', JSON.stringify(parsed))
    error.value = ''
    warn.value = ''
    ok.value = true
  } catch {
    validate(true)
  }
}

// 外部改值（例如切换编辑对象）时重新校验
watch(
  () => props.modelValue,
  () => validate(false)
)

/** 供父组件调用：提交前确保语法没问题 */
defineExpose({ validate })
</script>

<style scoped>
.rf-json-toolbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 4px;
}

.rf-json-textarea :deep(textarea) {
  font-family: Consolas, Monaco, 'Courier New', monospace;
  font-size: 12px;
  line-height: 1.6;
}
</style>
