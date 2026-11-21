# 玄览 0.10.9 发布说明

> 发布：2026-11-21 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.9 集中修复多词搜索的分隔符解析与多词高亮，并保留 0.10.8 的搜索自动分页加载。

安装包：`xuanlan_0.10.9_amd64.fpk`（1,231,338 字节）。
SHA-256：`42c502967fd3b4e02c782e785764957c29b359d822781573f5628211569fa30c`。

## 多词分隔符

- 支持空格；
- 支持英文逗号 `,`；
- 支持中文逗号 `，`；
- 支持顿号 `、`；
- 连续分隔符自动合并；
- 中英文混排；
- 按完整 UTF-8 字符消费，不再多跳字符。

修复：

```text
alpha,beta  → alpha + beta
测试，文档    → 测试 + 文档
测试、文档    → 测试 + 文档
alpha,,，、 测试 → alpha + 测试
```

## 多词高亮

- 搜索结果文件名分别高亮每个关键词；
- 引号精确词组整体高亮；
- `"alpha 测试"` 不会拆成两个高亮；
- 默认不区分英文大小写。

## 其他

- 单一命中 total 去重，重复关键词不重复计数；
- 0.10.8 的搜索自动分页加载继续保留：首屏 500 条，未取完自动加载；
- 团队独立索引、HLS 视频、PDF 显示全部、闲置扫描等保持。

## 回归

```text
markdown / smoke / api / scanmode / route / unitbuild / epoch
unitdelta / group-intersection / units-admin / repair
multisearch / idle-scan / ui
```

全部通过。ext4 镜像测试因本机缺少 `subnetdesk-build:bookworm` 未执行。

## 安装包

`xuanlan_0.10.9_amd64.fpk`

构建后补充字节数与 SHA-256。
