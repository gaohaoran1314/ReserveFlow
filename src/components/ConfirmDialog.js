// =============================================================================
// src/components/ConfirmDialog.js  —  ElMessageBox 统一封装
//
// 目的：把「二次确认」的文案风格、按钮顺序、危险色统一到一处，
// 避免各页面自己写 MessageBox 导致「有的确认按钮在左、有的在右」。
//
// 用法：
//   const ok = await confirm({ title: '删除资源', message: '...', danger: true })
//   if (!ok) return
// =============================================================================
import { ElMessageBox } from 'element-plus'

/**
 * @param {{ title?: string, message: string, confirmText?: string, cancelText?: string, danger?: boolean }} opts
 * @returns {Promise<boolean>} true=确认，false=取消（**不会 reject**，调用方无需 try/catch）
 */
export async function confirm(opts = {}) {
  const {
    title = '操作确认',
    message = '确认执行该操作？',
    confirmText = '确认',
    cancelText = '取消',
    danger = false,
  } = opts

  try {
    await ElMessageBox.confirm(message, title, {
      confirmButtonText: confirmText,
      cancelButtonText: cancelText,
      type: danger ? 'warning' : 'info',
      // danger 时确认按钮标红：用 customClass 交给全局样式/内联样式控制
      confirmButtonClass: danger ? 'el-button--danger' : '',
      closeOnClickModal: false,
      distinguishCancelAndClose: true,
    })
    return true
  } catch {
    // 'cancel'（点取消）与 'close'（点 X / ESC）都视为「不执行」
    return false
  }
}

/**
 * 只做提示（无取消按钮）
 * @param {string} message
 * @param {string} [title]
 */
export async function alertInfo(message, title = '提示') {
  try {
    await ElMessageBox.alert(message, title, { confirmButtonText: '知道了' })
  } catch {
    /* 用户直接关闭，忽略 */
  }
}

export default { confirm, alertInfo }
