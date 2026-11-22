# 玄览 0.10.15 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.15 调整播放器静音交互。

安装包：`xuanlan_0.10.15_amd64.fpk`（1,242,543 字节）。
SHA-256：`cb25686b0295eb99029e63dc05e3f463608bec606d468f8ea6603e5f11418553`。

## 音量

- 点击静音后音量滑块置灰并禁用；
- 取消静音后滑块恢复可用；
- 自动恢复静音前保存的音量；
- 音量与静音状态仍分别持久化：
  - `xl_video_volume`
  - `xl_video_muted`

## 回归

markdown / smoke / api / scanmode / route / unitbuild / epoch / unitdelta /
group-intersection / units-admin / repair / multisearch / pagination /
idle-scan / ui / ext4_image 全部通过。

## 安装包

`xuanlan_0.10.15_amd64.fpk`

构建后补充字节数与 SHA-256。
