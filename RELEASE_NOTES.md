# 玄览 0.10.10 发布说明

> 发布：2026-11-22 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.10 针对多词搜索稳定性和检索效率做一轮优化，并修复高亮与分隔符边界问题。

安装包：`xuanlan_0.10.10_amd64.fpk`（1,232,358 字节）。
SHA-256：`a262fecad05061cbbea05a4a0c2d392715f23dcd3fe0a1cfcc4d6b38e263a630`。

## 检索优化

- 多词“同时命中”不再固定用最长词出候选；
- 改为按每个词的最稀有 trigram 选择候选源；
- 稀有词优先，减少大索引上对常见词的无效扫描；
- 词项复核仍保持完整语义。

## 多词稳定性

- 支持空格、英文逗号 `,`、中文逗号 `，`、顿号 `、`、全角空格；
- 连续分隔符自动合并；
- 英文逗号不再多跳字符；
- 中文分隔符按完整 UTF-8 字符消费；
- 引号精确词组不拆分；
- 单一命中 total 去重；
- 自动加载无进展或失败即停止，避免重复请求。

## 高亮

- 结果文件名按每个词分别高亮；
- 引号词组整体高亮；
- 覆盖中文/英文混排、逗号、顿号、连续分隔符。

## 回归

```text
markdown / smoke / api / scanmode / route / unitbuild / epoch
unitdelta / group-intersection / units-admin / repair
multisearch / idle-scan / ui
```

全部通过。ext4 镜像测试因本机缺少 `subnetdesk-build:bookworm` 未执行。

## 安装包

`xuanlan_0.10.10_amd64.fpk`

构建后补充字节数与 SHA-256。
