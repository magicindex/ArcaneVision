# 玄览 / Arcane Vision

> 伫中区以玄览，颐情志于典坟。 —— 陆机《文赋》

飞牛 fnOS 上的全盘文件索引与毫秒级检索引擎。全新编写，不包含任何第三方项目的源代码。

作者：cyberzero　·　神念所至，万象玄览 / Spiritual Sense, All Things Revealed

## 功能

- **毫秒级检索**：直接读 ext4 元数据建索引，500 万条目规模下 3 字符以上查询通常 0.5–7 ms
- **多卷统一**：/vol1–/vol4 合并为一棵结果树，按修改时间归并
- **按账号授权**：身份取自飞牛会话（`X-Trim-Userid`），逐目录按 `system.trim_acl` 求值；
  家目录可开「严格模式」，忽略「所有人」授权
- **类型分类**：12 类图标与筛选条（文档/表格/演示/PDF/文本/代码/图片/视频/音频/压缩包…）
- **即时预览**：内置 PDF 阅读器（按页宽适配）、文本/代码高亮底色、图片音视频直接播放；
  可关联飞牛「文档服务」「在线预览」
- **文件操作**：多选复制/移动/回收站，回收站支持恢复与清空
- **索引自维护**：fanotify 实时增量 + 定时全盘对账，索引落盘，重启秒开
- **明暗双主题**：标题栏一键切换，偏好保存在本地
- **单文件 FPK**：随包提供 Web UI，安装后从飞牛桌面打开

## 界面

检索：输入即出结果，左侧为类型图标，右侧为大小与修改时间。

![检索](docs/screenshots/search.png)

右键菜单：打开、下载、定位到文件夹、复制/移动/回收站。

![右键菜单](docs/screenshots/context-menu.png)

定位到文件夹：在页内面板展开所在目录并高亮目标文件。

![定位到文件夹](docs/screenshots/finder.png)

PDF 预览：内置 pdf.js，按页宽适配，可拖拽缩放或最大化。

![PDF 预览](docs/screenshots/preview.png)

设置：打开位置、10 类格式关联、自动全盘扫描、检索范围与严格模式。

![设置](docs/screenshots/settings.png)

## PDF 阅读器

- 内置 pdf.js（Mozilla，Apache-2.0，见 `ui/vendor/LICENSE.pdfjs`），默认按页宽适配
- 设置齿轮图标取自 Feather Icons（MIT，见 `ui/vendor/LICENSE.feather-icons.txt`）
- 第三方组件与许可义务见 `THIRD_PARTY.md`，审计报告见 `docs/05-第三方与许可审计.md`
- 支持等比缩放、翻页；窗口拉宽/最大化后自动重排

## 文件操作

- 结果行勾选多选：复制到…、移动到…、移到回收站
- 回收站：右上角入口，支持恢复选中与清空；删除均为移入回收站，不直接销毁
- 目标目录选择器复用 `/api/dir`，只列可访问目录

## 现在是什么状态

当前版本 **0.6.12**，更新记录见 [CHANGELOG.md](CHANGELOG.md)。

P0–P3 已跑通：扫描（目录遍历 + ext4 裸设备）→ 建索引 → 落盘 → mmap 加载 → 查询
→ fanotify 增量 → TrimACL 求值 → HTTP 服务 + Web UI，并已打成可安装的飞牛 FPK。
全部是本仓库自己的代码，纯 C11 + glibc，服务端无第三方依赖、无 Qt。

ext4 裸设备读取器已用 `/dev/dm-1` 全卷验证：369 万条记录与 `find -xdev` 逐路径一致，
扫描 4.3 s（热）／33 s（无缓存），详见 `docs/03-P1实测.md`。

增量与权限的实现细节见 `docs/04-P2增量与ACL.md`。

## 服务托管

安装包自带 `xuanlan.service`：首次启动时同步到 `/etc/systemd/system` 并写入 `app.pid`。
应用中心的 `cmd/main` 把 `status / start / stop` 委派给 `systemctl`，两套入口指向同一个进程，
不会出现「应用中心显示已退出、实际还在跑」或两个实例抢 socket 的情况。

```bash
systemctl status xuanlan      # 查看状态
systemctl restart xuanlan     # 重启（索引从快照加载，秒开）
journalctl -u xuanlan -n 50   # 查看日志
```

## 构建

```
make            # 产出 bin/xuanlan
make install    # 装到 /usr/local/bin
```

只需要 gcc 或 clang。

## 用法

```bash
# 建索引
bin/xuanlan build --root /vol1/1000 --out /vol4/@appdata/xuanlan/index.xl \
                  --exclude /vol1/docker

# 直接从块设备读 ext4（需 root，路径前缀取挂载点或 --root）
bin/xuanlan build --device /dev/dm-1 --out /vol4/@appdata/xuanlan/vol1.idx -v

# 查询
bin/xuanlan query --index /vol4/@appdata/xuanlan/index.xl -n 20 报告
bin/xuanlan query --index /vol4/@appdata/xuanlan/index.xl --files --json .mkv

# 启动服务（HTTP + 网关 socket + Web UI）
bin/xuanlan serve --roots /vol1,/vol2,/vol3,/vol4 --index-dir /vol4/@appdata/xuanlan

# 判定某用户能否列某目录（TrimACL 求值）
bin/xuanlan acl 900 /vol1/1000 /vol1/vm

# 看索引元数据 / 导出全部路径
bin/xuanlan info --index /vol4/@appdata/xuanlan/index.xl
bin/xuanlan dump --index /vol4/@appdata/xuanlan/index.xl
```

查询串是大小写不敏感的子串匹配，中文按 UTF-8 字节处理，无需分词。

## 实测基线（飞牛 fnOS 环境，2026-11）

`/vol1/@appcenter`，173,128 条记录：

| 项 | 结果 |
|---|---|
| 扫描（页缓存热） | 253 ms |
| 建 trigram 索引 | 424 ms |
| 倒排编码后体积 | 3.5 MB（**1.29 B/posting**） |
| 快照文件 | 10.1 MB |
| 全流程 | 686 ms |
| 查询 `screenshot` / `docker-compose` | < 1 ms |
| 查询单字符 `a`（名字池扫描） | 18 ms |

与 `find` 逐条比对：记录数与 `find -xdev` 完全一致（173,128），
12 个 `docker-compose` 命中一致，输出路径全部存在。

`/vol1` 全卷（`/dev/dm-1`，369 万条）：扫描 4.3 s、建索引 8.8 s、快照 247 MB，
常见词查询 1–7 ms，`json`（9.2 万命中）62 ms。

冷建瓶颈是 ext4 元数据的寻道次数，不是吞吐（见 `docs/01-架构.md`）。

## 目录

```
src/xl.h          公共类型、快照头、接口
src/util.c        内存、ASCII 折叠、大小写不敏感查找
src/scan.c        getdents64 扫描器（DFS 先序）
src/index.c       trigram 稀疏表 + 分块位打包倒排
src/snapshot.c    单文件快照，4KB 对齐段，mmap 直用
src/query.c       候选求交、全名复核、Top-K、路径还原
src/fsreader_ext4.c  ext4 裸设备读取（inode 位图 + extent 树 + 物理序目录块）
src/httpd.c       HTTP/unix socket 服务、多卷快照、增量合并
src/live.c        fanotify 增量（DFID 句柄还原 + delta 表）
src/perm.c        TrimACL 二进制求值 + 继承 + POSIX 回落
src/main.c        CLI / serve
docs/             架构、合规与净室说明
CHANGELOG.md      版本更新记录
tests/smoke.sh    冒烟测试
tests/ext4_image_test.sh  裸设备读取器与 getdents/find 的一致性测试
pkg/              FPK 打包源（manifest、cmd、wizard、ui、config）
scripts/build-fpk.sh      打包装包
```

## 路线图

- **P0 已在跑**：扫描、索引、快照、查询、CLI
- **P1**：ext4 裸设备扫描器（按公开的 on-disk 格式规范自行实现，顺序读 inode 表后按物理块推进）——把冷建从 15 分钟压到 2.5–4 分钟
- **P2 已完成**：fanotify 文件系统标记增量 + 周期对账（默认 6 小时）
- **P3 部分完成**：TrimACL 求值已实现并通过样本验证；多用户可见段与身份绑定待网关身份来源确认
- **P4 已完成**：Web UI、FPK 打包；已在飞牛 fnOS 真机安装运行，登录身份与目录授权均取自系统

## 许可

本项目以 **GPL-3.0** 发布，全文见 [LICENSE](LICENSE)。

- 第三方组件：pdf.js（Apache-2.0）、Feather Icons 图标路径（MIT），
  其版权与许可文本见 [THIRD_PARTY.md](THIRD_PARTY.md)、[NOTICE.md](NOTICE.md)，随二进制包分发；
- 仅在自行部署（不对外分发二进制）时，GPL 不施加额外义务；
- 分发二进制时须提供对应源码，获取地址见 [NOTICE.md](NOTICE.md)。

## 合规

本项目为净室重写：未复制、未改编任何现有搜索引擎（包括 Everything、kerything）的源代码，
仅在阅读公开文档与公开接口的基础上自行实现。细节与依赖清单见 [docs/02-版权与净室.md](docs/02-版权与净室.md)。
