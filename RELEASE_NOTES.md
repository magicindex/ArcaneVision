# 玄览 0.10.20 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.20 修复播放器窗口最小高度出现滚动条的问题。

安装包：`xuanlan_0.10.20_amd64.fpk`（1,243,344 字节）。
SHA-256：`d457d38ee5e22b5650f5dc68d717af89a408a228f8a8f521c7fb5c936beeef52`。

## 窗口

- 最小尺寸 600×420；
- 最小高度下控件完整可见；
- 隐藏横/纵滚动条；
- 初始化和恢复保存夹取后的尺寸；
- 记住上次大小，最小不低于 600×420；
- 视口缩放自动夹取；
- 底部控制条不截断。

## 验证

```text
markdown / smoke / api / scanmode / route / unitbuild / epoch
unitdelta / group-intersection / units-admin / repair
multisearch / pagination / idle-scan / ui / ext4_image
```

全部通过。

## 安装包

`xuanlan_0.10.20_amd64.fpk`

构建后补充字节数与 SHA-256。
