# 玄览 0.10.64 发布说明

> 发布：2026-11-27 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.64 修复部分机型设置面板无法上下滑动。

安装包：`xuanlan_0.10.64_amd64.fpk`（2,197,786 字节）。
SHA-256：`c8dec30fbca55309cba748f7ed2afc9960e9cc8aaf7dcab36b4c8d999b3980f5`。

## 问题

- 荣耀 Play 5 Pro 等机型在飞牛 App 内打开「设置」时面板能显示，但内容无法上下滑动；
- 原因：全屏遮罩 `.setmask` 使用 `backdrop-filter: blur(6px)` 毛玻璃，
  部分 Android WebView 在毛玻璃合成层下会吞掉内部 `overflow` 的触摸滚动事件；
- 小米等设备毛玻璃实现正常，因此不易复现。

## 修复

- 手机 / 触屏下禁用设置、目录查找、弹窗遮罩的 `backdrop-filter`（背景色保留，视觉基本一致）；
- 设置面板 `.setbody`、目录列表补齐 `-webkit-overflow-scrolling: touch` 与 `touch-action: pan-y`；
- 遮罩与滚动容器增加 `overscroll-behavior: contain`，避免滚动穿透到页面；
- 桌面端仍保留毛玻璃效果。

## 验证

- 手机媒体查询下 `.setmask`/`.finder`/`.modal` 的 `backdrop-filter` 为 `none`；
- `.setbody` 计算样式包含 `touch-action: pan-y`、`overflow-y: auto`；
- `tests/ui_behavior_test.py` 通过。

## 安装包

`xuanlan_0.10.64_amd64.fpk`

SHA-256：`c8dec30fbca55309cba748f7ed2afc9960e9cc8aaf7dcab36b4c8d999b3980f5`。
