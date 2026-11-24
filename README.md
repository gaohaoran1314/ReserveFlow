# ReserveFlow 学生端 H5（front/h5）

Vue 3 + Vite 7 + **Vant 4** 的移动端预约端（375 宽度基准），对接 `back/ReserveFlow` 的 REST API。

## 一、启动

```bash
# 先启动后端
cd back/ReserveFlow && ./rfbuild/reserveflow.exe

cd front/h5
npm install          # 首次需联网（79 个包 / 约 46 秒）
npm run dev          # http://localhost:5174  （5173 是 admin 开发服）
```

演示账号：`student01 / User@123456`（正常）、`student02 / Black@123456`（黑名单，能登录但首页挂提示条）。

## 二、构建

```bash
npm run build        # 产 dist/，约 2 秒、638KB
npm run preview      # http://localhost:4174（带 /api 代理）
npm run check        # 移动端布局自检（CDP 375×812，7 条路由 + 截图）
```

产物特性（离线可交付）：

- `base: './'` → `./assets/...` 相对路径，可挂在 `/h5` 等任意子路径；
- `dist/index.html` **无任何 http(s) 外链**（无 CDN、无外部字体，图标用 emoji + Vant 内置图标字体，字体文件随包）；
- 路由 **Hash 模式**，深链刷新不依赖后端 fallback。

## 三、目录约定

```
src/
├── api/          # http.js（Vant 版拦截器）+ 5 个域文件
├── components/   # StatusTag / ResourceCard / ReservationCard / EmptyState
├── constants/    # enums（只 5 组）/ errorCode（Vant 呈现方式）/ storage（**独立 key**）
├── router/       # 7 条路由 + guard
├── stores/       # auth（登录态）/ dict（meta 缓存）
├── utils/        # format.js（字符串格式化）+ datetime.js（时间运算/倒计时）
└── views/        # 7 个页面
```

## 四、三条硬约定

1. **localStorage key 必须与 admin 隔离**：`rf_h5_token` / `rf_h5_user`（admin 是 `rf_admin_token` / `rf_admin_user`）。
   两个前端部署在同一域名下时 localStorage 同源，key 相同会让 H5 读到管理员的 token ——
   页面能进但"我的预约"永远是空的。见 `constants/storage.js`。
2. **时间解析用本地时区构造**（`utils/datetime.js` 的 `parseLocalDateTime`），
   不要用 `new Date('2026-11-23T09:00:00')` —— 那会按 UTC 解析，东八区整体偏移 8 小时。
3. **列表页的主操作按钮按 `status` 推导**（列表接口不返回 `allowedActions`），
   点进详情后**一律以服务端的 `allowedActions` 为准**。

## 五、当前进度

| 批次 | 内容 | 状态 |
| --- | --- | --- |
| T01 | 脚手架与构建 | 已完成 |
| T02 | 路由 + 底部 TabBar + 守卫 | 已完成 |
| T03 | HTTP 客户端 + 枚举 + auth store | 已完成 |
| T04 | 登录闭环（含黑名单提示条） | 已完成 |
| T05 | 字典 store + 通用组件 + 首页资源卡片 | 已完成 |
| T06~T09 | 首页收尾 / 时段选择器 / 资源详情下单 / 我的预约 | 已完成 |
| T10 | 候补页（offer 倒计时） | 已完成 |
| T11 | 信用页 / 我的页 | 已完成 |
| T12 | `/h5` 静态托管 + `mobile-check.mjs` + 全量回归 | 已完成 |

## 六、`/h5` 单进程演示

```bash
cd front/h5 && npm run build
cd ../../back/ReserveFlow && cmake -S . -B rfbuild && cmake --build rfbuild
./rfbuild/reserveflow.exe
# 打开 http://localhost:8080/h5/
```

启动日志会出现「用户端 H5 已挂载：/h5 → …」。产物缺失时只 WARN，服务照常启动。

## 七、移动端布局自检

```bash
npm run dev            # 或先用后端托管 /h5
npm run check          # 默认量 5174；--base http://127.0.0.1:8080/h5 可量托管形态
```

用 CDP 的 `Emulation.setDeviceMetricsOverride {375, 812, dpr:2, mobile:true}` 真正拿到 375 视口
（`--window-size=375,812` 实测给的是 492，不可用），逐路由量 `clientWidth` / `scrollWidth` /
溢出元素，并截图到 `scripts/shots/`。
