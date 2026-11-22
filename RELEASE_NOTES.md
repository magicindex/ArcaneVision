# 玄览 0.10.14 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.14 增强内置/胶囊播放器的音量控制。

安装包：`xuanlan_0.10.14_amd64.fpk`（1,242,534 字节）。
SHA-256：`7d3e987870a9b567b9b46a29598a4881385d599f21aaa9a130069ead9a58f2c8`。

## 音量控制

- 胶囊控制条显示音量滑块和静音按钮；
- 拖动音量滑块实时生效；
- 静音按钮可切换静音/恢复；
- 音量保存到 `xl_video_volume`；
- 静音状态单独保存到 `xl_video_muted`；
- 打开视频或刷新页面后恢复上次音量；
- 静音时滑块仍显示恢复音量，取消静音后恢复。

## 延续

固定背景、增量列表、分页 total、多词搜索、闲置扫描、ext4 回归保持。

## 回归

```text
markdown / smoke / api / scanmode / route / unitbuild / epoch
unitdelta / group-intersection / units-admin / repair
multisearch / pagination / idle-scan / ui / ext4_image
```

全部通过。

## 安装包

`xuanlan_0.10.14_amd64.fpk`

构建后补充字节数与 SHA-256。
