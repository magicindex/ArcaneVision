# 玄览 0.10.12 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.12 优化长列表搜索结果滚动：固定背景只随视口绘制，结果行增量追加并启用内容可见性优化。

安装包：`xuanlan_0.10.12_amd64.fpk`（1,232,632 字节）。
SHA-256：`c9bcb943a639278c10d54c5f68a7085c04aef85df9a6726ff93649fdd06384d5`。

## 滚动优化

- 固定 `body::before` 背景层，背景不随列表滚动；
- 搜索结果分页改为增量追加 DOM，不再每页重建整个列表；
- `li.row { content-visibility:auto; contain-intrinsic-size:auto 58px }`
  只绘制视口内行，降低长列表滚动渲染开销；
- 列表加载失败/无新数据立即停止自动请求，避免反复渲染。

## 搜索结果

- 首屏 500 条，自动加载后续页；
- 显示「正在加载更多… X / Y」；
- 加载完成或失败保留继续加载入口；
- 背景固定，只有文字内容滚动。

## 回归

```text
markdown / smoke / api / scanmode / route / unitbuild / epoch
unitdelta / group-intersection / units-admin / repair
multisearch / idle-scan / ui / ext4_image
```

全部通过。

## 安装包

`xuanlan_0.10.12_amd64.fpk`

构建后补充字节数与 SHA-256。
