# 玄览 0.10.69 发布说明

> 发布：2026-11-27 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.69 修复手机端「玄览」下方缺失英文副标题。

安装包：`xuanlan_0.10.69_amd64.fpk`（2,198,638 字节）。
SHA-256：`a11e41fda85a57d7b6b7a4ebd8507fc908cbf1431cbceba32f8783973ad4516d`。

## 问题

- 0.10.66 起手机品牌区恢复「玄览 + ARCANE VISION」，但 ≤380px 窄屏仍隐藏副标题；
- 荣耀 Play 5 Pro 等 360px 级视口下，「玄览」下面没有 ARCANE VISION。

## 修复

- ≤380px 恢复显示副标题：`ARCANE VISION` 6px、字距 1px，紧贴「玄览」下方；
- 390px 保持 7px，桌面端不变；
- 360px 实测 header 无横向溢出。

## 验证

- 360px：副标题 `display:block`、字号 6px、无溢出；
- 390px：字号 7px、无溢出；
- `tests/ui_behavior_test.py` 通过。

## 安装包

`xuanlan_0.10.69_amd64.fpk`

SHA-256：`a11e41fda85a57d7b6b7a4ebd8507fc908cbf1431cbceba32f8783973ad4516d`。
