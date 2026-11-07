# 第三方组件与许可

本文件列出玄览（xuanlan）分发物中包含的第三方内容。凡未列出的代码均为本项目原创。

## 运行时分发

| 组件 | 版本 | 许可证 | 用途 | 修改 | 位置 |
|---|---|---|---|---|---|
| pdf.js (pdfjs-dist) | 4.10.38 | Apache-2.0 | 浏览器内 PDF 渲染（按页宽适配） | 未修改 | `ui/vendor/pdf.min.mjs`、`ui/vendor/pdf.worker.min.mjs`、许可证 `ui/vendor/LICENSE.pdfjs` |
| fflate | 0.8.2 | MIT | 浏览器内解压 EPUB（ZIP/DEFLATE） | 未修改（UMD 构建） | `ui/vendor/fflate.min.js`、许可证 `ui/vendor/LICENSE.fflate.txt` |
| Feather Icons | — | MIT | 设置齿轮图标路径（24×24 线框） | 仅引用路径数据 | `ui/index.html`，许可证 `ui/vendor/LICENSE.feather-icons.txt` |

## 系统库（不随包分发）

| 组件 | 许可证 | 说明 |
|---|---|---|
| glibc | LGPL-2.1-or-later（含链接例外） | 动态链接 `libc.so.6`，使用系统调用与标准 C 库；按 LGPL 例外条款分发 |
| Linux 内核接口 | GPL-2.0（内核自身） | 只通过 syscall（fanotify、getdents64、pread 等）调用，不构成衍生作品 |

## 明确未使用

- **kerything**（GPL-3.0）：项目早期评估过其架构，最终未复用一行代码，仓库内没有 C++ 源文件。
- **libext2fs / e2fsprogs**（LGPL-2.0）：ext4 读取器按公开 on-disk 格式规范自行实现，未链接、未复制。
- **Qt / GTK / TBB** 等 UI 与并行框架：未使用。
- **Everything（voidtools）**：未使用其二进制、界面资源或名称。
- **fnOS 二进制**（`/usr/trim/bin/*`）：未反编译、未复制代码；仅通过 strace、strings、xattr 样本与前端 bundle 观察外部行为。

## 兼容性实现说明

以下能力是按“可观察的外部行为”重新实现的，不含对方源代码：

- ext4 磁盘格式：依据公开规范（superblock、inode table、extent tree、dir_entry_2）。
- TrimACL 扩展属性 `system.trim_acl`：通过读写样本与 `trimacl` CLI 差分测试推导字段语义。
- 飞牛网关请求头（`X-Trim-Userid` 等）与 `/appview` 深链参数：通过实际请求观察。

这类互操作性实现通常不构成对原程序的复制，但**不等于获得官方许可**：发布时应避免使用飞牛或 OnlyOffice 的商标与图标，也不要以“官方应用”名义传播。

## 我们的许可

本项目以 **GPL-3.0** 发布（见 `LICENSE`）。

兼容性说明：GPLv3 与 Apache-2.0（pdf.js）兼容，可从 Apache-2.0 项目取用并整体以 GPLv3 分发；
MIT（fflate、Feather）同样兼容。因此本项目的最终分发物可以合法地以 GPLv3 覆盖，同时保留各第三方组件的原许可声明。
