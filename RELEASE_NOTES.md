# 玄览 0.10.21 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.21 修复 HLS 播放失败时 ffmpeg/NVENC 持续占用导致 GPU 满载的问题。

安装包：`xuanlan_0.10.21_amd64.fpk`（1,245,366 字节）。
SHA-256：`d3ba62e1504de72061040c2b62fe3072612ecc18e2a05d792af12040be55c1e0`。

## 视频播放

- HLS 支持检测失败时直接走直出转码，不启动 HLS；
- HLS 启动 10 秒无播放自动回退直出转码；
- HLS fatal error 立即回退；
- 回退/关闭时调用 `/api/hls/stop` 杀掉 ffmpeg；
- 同路径新会话先清理旧会话；
- HLS 会话 120 秒无请求自动回收。

## 回归

markdown / smoke / api / scanmode / route / unitbuild / epoch / unitdelta /
group-intersection / units-admin / repair / multisearch / pagination /
idle-scan / ui / ext4_image。

## 安装包

`xuanlan_0.10.21_amd64.fpk`

构建后补充字节数与 SHA-256。
