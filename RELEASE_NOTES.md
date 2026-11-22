# 玄览 0.10.22 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.22 修复播放器窗口 CSS 错误导致双击视频无画面。

安装包：`xuanlan_0.10.22_amd64.fpk`（1,245,348 字节）。
SHA-256：`4623edb62c8d93e66c1b92be92f80a3ce6f04922da85cbf80e6d95eeab0da5a5`。

## 修复

- `.openwin` 的 min-width/min-height 样式补回缺失右括号；
- `.openwin.on{display:flex}` 恢复生效；
- 双击视频正常显示播放器；
- 直出转码播放成功；
- HLS 转码播放成功；
- 控制条完整可见。

## 验证

- Chromium 真实页面加载；
- 双击 `披萨创意广告大比拼.mp4` 打开 HLS；
- 视频 `readyState=4`、`paused=false`；
- 控制台无报错；
- 截图确认画面和控制条可见。

## 安装包

`xuanlan_0.10.22_amd64.fpk`

构建后补充字节数与 SHA-256。
