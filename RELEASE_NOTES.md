# 玄览 0.10.17 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.17 修复播放器图标初始化顺序导致的设置面板无法打开。

安装包：`xuanlan_0.10.17_amd64.fpk`（1,242,990 字节）。
SHA-256：`c302a75f1e748e8a34c4989ef2423177dc44b0afa7dd8b83006591a2b2cfcb37`。

## 修复

- 移除 `const vcompact` 声明前对 `vcompact` 的访问；
- 修复页面初始化 `ReferenceError: Cannot access 'vcompact' before initialization`；
- 设置齿轮恢复响应；
- 播放器图标正常初始化。

## 回归

markdown / smoke / api / scanmode / route / unitbuild / epoch / unitdelta /
group-intersection / units-admin / repair / multisearch / pagination /
idle-scan / ui / ext4_image。

## 安装包

`xuanlan_0.10.17_amd64.fpk`

构建后补充字节数与 SHA-256。
