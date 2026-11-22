# 玄览 0.10.13 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.13 修复长列表搜索结果 total 与分页不一致，并优化滚动内存/渲染开销。

安装包：`xuanlan_0.10.13_amd64.fpk`（1,242,264 字节）。
SHA-256：`af8b733dcd2067345c08e0848ac3fe9d1a2a7d425740dd0e61f4060b97701d2d`。

## 分页与 total

- 修复 live 增量与基线快照重复记录导致 total 虚高；
- 去重后 total 与可分页结果一致；
- 新增分页测试：1200 条记录，total=1200，三页无重复无丢失。

## 滚动与内存

- 固定背景层 `body::before`，仅内容滚动；
- 搜索结果分页增量追加 DOM，不重建整表；
- `content-visibility:auto` 只绘制视口附近行；
- 自动加载失败/无进展立即停止。

## 回归

```text
markdown / smoke / api / scanmode / route / unitbuild / epoch
unitdelta / group-intersection / units-admin / repair
multisearch / pagination / idle-scan / ui / ext4_image
```

全部通过。

## 安装包

`xuanlan_0.10.13_amd64.fpk`

构建后补充字节数与 SHA-256。
