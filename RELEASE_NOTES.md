# 玄览 0.10.54 发布说明

> 发布：2026-11-24 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.54 修复经 fnOS 网关注入路径下的赞赏码图片 404。

安装包：`xuanlan_0.10.54_amd64.fpk`（2,188,166 字节）。
SHA-256：`9bf10f49a5a0dd8f4770f8780d44b2a5673c2a64556bf53559fd76db6276518e`。

## 问题

- 应用中心、桌面入口或直接访问 `http://<NAS>:5666/app/xuanlan`（无尾斜杠）时，
  页面内相对路径 `assets/donation-*.jpg` 会被浏览器解析为 `/app/assets/…`；
- 该路径不在应用代理范围内，返回 404，赞助弹窗中的微信/支付宝赞赏码显示为破图；
- 通过 `http://<NAS>:8971/` 直连时路径恰好正确，因此本地验证不易发现。

## 修复

- 赞赏码图片改用页面 `ROOT` 绝对路径：
  - 经网关打开：`/app/xuanlan/assets/donation-*.jpg`
  - 直接访问 8971：`/assets/donation-*.jpg`
- `ROOT` 与页面 `API` 变量使用同一套路径推断，兼容 `/app/xuanlan`、`/app/xuanlan/`、
  `/app/xuanlan/index.html` 三种入口。

## 验证

- 网关日志中 `GET /app/assets/donation-wechat.jpg` 404 不再出现；
- 新路径请求返回 `200 image/jpeg`，两张赞赏码大小分别为 145,207 / 133,044 字节；
- 暗色 / 亮色、桌面 / 手机弹窗渲染正常。

## 安装包

`xuanlan_0.10.54_amd64.fpk`

SHA-256：`9bf10f49a5a0dd8f4770f8780d44b2a5673c2a64556bf53559fd76db6276518e`。
