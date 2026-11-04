# 玄览 / Arcane Vision

> 伫中区以玄览，颐情志于典坟。 —— 陆机《文赋》

飞牛 fnOS 上的全盘文件索引与毫秒级检索引擎。全新编写，不包含任何第三方项目的源代码。

作者：cyberzero　·　神念所至，万象玄览 / Spiritual Sense, All Things Revealed

## 功能

- **毫秒级检索**：直接读 ext4 元数据建索引，500 万条目规模下 3 字符以上查询通常 0.5–7 ms；
  索引记录文件大小与 mtime，结果按真实修改时间倒序
- **多卷统一**：/vol1–/vol4 合并为一棵结果树，按修改时间归并
- **按账号授权**：unix socket 上的飞牛身份头才可信，TCP 必须 Cookie 认证，否则 `/api/*` 一律 401；
  逐目录按 `system.trim_acl` 求值，家目录可开「严格模式」，忽略「所有人」授权
- **类型分类**：12 类图标与筛选条（文档/表格/演示/PDF/文本/代码/图片/视频/音频/压缩包…）
- **即时预览**：内置 PDF 阅读器（按页宽适配）、文本/代码高亮底色、图片音视频直接播放；
  可关联飞牛「文档服务」「在线预览」；服务端转码支持 HDR→SDR 与纯音频
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

当前版本 **0.8.1**，更新记录见 [CHANGELOG.md](CHANGELOG.md)。支持明暗双主题，默认使用飞牛账号身份与系统目录授权。
0.8.1 修复 SMB/ZFile 迁移目录中 mode 000 文件被误过滤、目录显示为空的问题。
### 0.8.1 修复内容

- 修复 SMB/ZFile 迁移目录中文件模式为 `000`、但属主经 TRIM ACL 仍可读时，
  目录列表把文件全部过滤掉、界面显示「该文件夹没有可见内容」的问题。
- 请求不存在的目录返回 `not found`，不再返回空列表；搜索会跳过已删除或已卸载的残留索引路径。
- 0.8.1 不改变索引格式（仍为 v2），从 0.8.0 升级不需要重新全量重建。

0.8.0 起索引升级到 v2（记录真实 `size/mtime`），从 0.7.x 升级后首次启动会自动全量重建一次；
按 4.9M 条实测约 25–30 分钟，重建期间服务可响应但结果会暂时变少。升级前建议备份 `idx` 目录。

P0–P6 已跑通：扫描（目录遍历 + ext4 裸设备）→ 建索引 → 落盘 → mmap 加载 → 查询
→ fanotify 增量 → TrimACL 求值 → HTTP 服务 + Web UI，并已打成可安装的飞牛 FPK。
全部是本仓库自己的代码，纯 C11 + glibc，服务端无第三方依赖、无 Qt。

ext4 裸设备读取器已用 `/dev/dm-1` 全卷验证：369 万条记录与 `find -xdev` 逐路径一致，
扫描 4.3 s（热）／33 s（无缓存），详见 `docs/03-P1实测.md`。

增量与权限的实现细节见 `docs/04-P2增量与ACL.md`。

## 已知问题与限制

以下问题已知、未隐藏，按影响从大到小排列。欢迎在 Issue 里补充复现条件。

**安全**

- 8971 端口只绑定 `127.0.0.1`。身份头（`X-Trim-Userid` 等）只在 unix socket 上可信；
  TCP 连接必须通过 Trim 登录 Cookie 认证，否则所有 `/api/*` 返回 401（fail-closed）。
  生产环境仍建议通过 NAS 网关（unix socket，已做 `SO_PEERCRED` 校验）访问。
- 状态变更接口（重建索引、全局设置、清空回收站）仅管理员可用；设置/文件操作/回收站写接口
  要求 `Content-Type: application/json`，防止简单表单 CSRF。
- 索引文件不加密。能读到 `<数据目录>/idx` 的人可以看到文件名、路径、大小与修改时间，但看不到文件内容。
- `serve --no-tcp` 可完全关闭回环 TCP，仅保留 unix socket；经 NAS 网关部署时建议启用。
- 索引 v2 记录真实 size/mtime；旧 v1 索引启动时会被拒绝并触发一次全量重建。

**能力边界**

- 不索引 SMB/CIFS 挂载与 `/fs`（trimafs）；网络共享需要先挂到本地卷。
- 不索引文件内容（没有全文检索）；仅按文件名、类型、路径检索。
- 服务端转码支持纯音频与 HDR→SDR（需要 ffmpeg 的 zscale 滤镜）；HDR 转换失败时回退为
  普通 8-bit 转换，颜色可能偏淡。
- 索引重建期间可能短暂保留已删除或已卸载的路径。查询会对结果做 `stat` 过滤，目录请求对
  不存在的路径返回 `not found`，但重建完成前目录视图仍可能有极短的旧记录窗口。
- 回收站不会自动过期清理，空间需要手动回收。
- 目录列举与「检索范围」授权列表仍按目录逐条扫描快照，复杂度为 O(N)；4.9M 条规模下单目录
  约 78 ms，设置页展开大量根目录时可能到秒级。后续索引会补目录/子项索引。
- 冷启动建索引耗时取决于卷类型：ext4 约 430 秒/370 万条；ZFS 因 `d_type` 不可用需逐条
  `fstatat`，22T 卷约 15–20 分钟。4.9M 条全量实测 25–30 分钟；热启动从快照加载通常 1 秒内可用。

**与飞牛的集成**

- 飞牛没有对外可用的「文件管理」URL，右键「定位到文件夹」只能打开应用内定位面板，
  不能唤起系统文件管理器窗口。
- 「文档服务」「在线预览」的跳转 URL 依赖这两个应用的实现，飞牛版本升级后可能变化，
  失效时会回落到内置 PDF 阅读器或 `/api/file`。
- 权限判定依赖 `system.trim_acl` 与属主规则；飞牛若调整这套语义，需要同步跟进。
- SMB/ZFile 迁移文件常以 `000` 模式出现，实际可读性由属主和 TRIM ACL 决定。0.8.1 已按显式
  ACL 与属主判定；非属主且 ACL 未授权的账号仍会被拒绝。

**部署**

- 以 root 运行（读块设备、读 `system.trim_acl`、fanotify 都需要权限）。
- 单机单进程，没有多副本与集群；索引不跨机器共享。

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

# 启动服务（网关 socket + Web UI）；--no-tcp 可完全关闭回环 8971
# --exclude 可重复；存在排除项时 ext4 裸扫会自动退回目录遍历
bin/xuanlan serve --roots /vol1,/vol2,/vol3,/vol4 --index-dir /vol4/@appdata/xuanlan \
                 --exclude /vol1/docker --no-tcp

# 判定某用户能否列某目录（TrimACL 求值）
bin/xuanlan acl 900 /vol1/1000 /vol1/vm

# 看索引元数据 / 导出全部路径
bin/xuanlan info --index /vol4/@appdata/xuanlan/index.xl
bin/xuanlan dump --index /vol4/@appdata/xuanlan/index.xl
```

查询串是大小写不敏感的子串匹配，中文按 UTF-8 字节处理，无需分词；少于 2 个字符的查询会直接拒绝。

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
| 单字符查询 | 直接拒绝（避免全 names 池扫描） |

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

- **P0 已完成**：扫描、索引、快照、查询、CLI
- **P1 已完成**：ext4 裸设备扫描器（按公开的 on-disk 格式规范自行实现，顺序读 inode 表后按物理块推进），冷建从 15 分钟降到约 7 分钟/370 万条
- **P2 已完成**：fanotify 文件系统标记增量 + 周期对账（默认 6 小时）
- **P3 已完成**：TrimACL 求值通过样本与真机验证；身份取自飞牛会话头 `X-Trim-Userid`，多用户结果按权限过滤
- **P4 已完成**：Web UI、FPK 打包；已在飞牛 fnOS 真机安装运行，登录身份与目录授权均取自系统
- **P5 已完成（0.7.x）**：明暗双主题、安全加固（socket 对端校验、软链逃逸、回收站路径）、
  服务生命周期与应用中心状态对齐
- **P6 已完成（0.8.0）**：索引 v2 记录真实 `size/mtime` 并按真实时间排序；目录增量子树；
  fail-closed 身份与文件级读写权限；HDR/纯音频转码；`--no-tcp`；查询 Top-K 与快照加固
- **下一步（0.9.x）**：索引加密、SMB/CIFS 与全文检索、目录/ACL 的 O(N) 优化

## 许可

本项目以 **GPL-3.0** 发布，全文见 [LICENSE](LICENSE)。

- 第三方组件：pdf.js（Apache-2.0）、Feather Icons 图标路径（MIT），
  其版权与许可文本见 [THIRD_PARTY.md](THIRD_PARTY.md)、[NOTICE.md](NOTICE.md)，随二进制包分发；
- 仅在自行部署（不对外分发二进制）时，GPL 不施加额外义务；
- 分发二进制时须提供对应源码，获取地址见 [NOTICE.md](NOTICE.md)。

## 合规

本项目为净室重写：未复制、未改编任何现有搜索引擎（包括 Everything、kerything）的源代码，
仅在阅读公开文档与公开接口的基础上自行实现。细节与依赖清单见 [docs/02-版权与净室.md](docs/02-版权与净室.md)。
