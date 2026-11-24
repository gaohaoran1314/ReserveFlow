<template>
  <el-config-provider :locale="zhCn">
    <router-view />
  </el-config-provider>
</template>

<script setup>
// 全局中文语言包（分页、表格、日期选择等的内置文案）
import zhCn from 'element-plus/es/locale/lang/zh-cn'
</script>
