# 玄览 0.10.19 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.19 调整播放器窗口尺寸，保证控制条完整可见。

安装包：`xuanlan_0.10.19_amd64.fpk`（1,243,296 字节）。
SHA-256：`17e0659561360a8d7f792e2252ec2aa0e12da64c473194038533da371c0b67cc`。

## 窗口尺寸

- 初始默认最小 560×360；
- 旧尺寸小于最小值时自动夹取；
- 最小尺寸按初始化默认最小，不允许更小；
- 记住上次窗口大小；
- 视口变化自动夹取；
- `overflow:hidden`，不显示滚动条；
- 底部控制条完整可见。

## 回归

markdown / smoke / api / scanmode / route / unitbuild / epoch / unitdelta /
group-intersection / units-admin / repair / multisearch / pagination /
idle-scan / ui / ext4_image。

## 安装包

`xuanlan_0.10.19_amd64.fpk`

构建后补充字节数与 SHA-256。
