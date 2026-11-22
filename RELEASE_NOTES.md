# 玄览 0.10.23 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.23 修复 MKV 等容器无法直接播放的问题。

安装包：`xuanlan_0.10.23_amd64.fpk`（1,245,484 字节）。
SHA-256：`6c57629e39d62dd4ea4cad07491688280388a7a62fe29b4a22fdf8b3b8fe36c5`。

## MKV/容器

- MKV、AVI、WMV、FLV、TS、M2TS、RMVB、3GP 识别为浏览器不可靠容器；
- 即使内部是 H.264 也强制走转码；
- H.265/HDR/10bit/4K MKV 继续 HLS 1080p 转码；
- Chromium 实测 MKV：
  - `openwin on`
  - `readyState=4`
  - `paused=false`
  - 无控制台错误。

## 安装包

`xuanlan_0.10.23_amd64.fpk`

构建后补充字节数与 SHA-256。
