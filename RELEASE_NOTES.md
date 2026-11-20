# 玄览 0.10.7 发布说明

> 发布：2026-11-21 · 适用于飞牛 fnOS（amd64）· 正式版

0.10.7 增加多词搜索与「高级搜索」设置面板，并保留 0.10.6 的团队独立索引、HLS 视频、
PDF「显示全部」、分区域扫描统计和板块级重建隔离。

安装包：`xuanlan_0.10.7_amd64.fpk`（1,231,355 字节）。
SHA-256：`8983ca5a2ea7daebfbbecbd7fe672e46ebaa793a2196c0fcb0e8fb756ff97bb8`。

## 多词搜索

- 多个关键词用空格分隔，默认要求**同时命中**；
- 可切换「单一命中」，任一词出现即返回；
- 双引号内保留空格作为**精确词组**，例如 `"量子 计算"`；
- 支持中文 / 英文混合；
- 分隔方式可选：空格、空格或逗号、空格或换行；
- 可配置区分英文大小写；
- 可配置按出现顺序匹配，默认任意顺序；
- 设置页最下方新增「高级搜索」面板，附示例和详细说明。
- 自动全盘扫描支持「自定义周期」和「闲置时扫描」，默认关闭；
- 闲置扫描检查 CPU 负载、可用内存、磁盘 IO，连续空闲 60 秒且 60 秒内无查询才启动。

详细语法见 [docs/11-多词搜索.md](docs/11-多词搜索.md)。

## 设置与 API

`/api/settings` 新增并持久化：

```json
{
  "searchMulti": true,
  "searchMatchAll": true,
  "searchSeparator": "space",
  "searchCaseSensitive": false,
  "searchOrdered": false
}
```

`searchSeparator`：`space` / `comma` / `line`。

## 重点更新（延续 0.10.6）

- 团队文件独立 `team-volN.idx`；
- 本地 / 团队 / 外接 / 远程四板块独立重建；
- 定时重建覆盖全部板块；
- 分区域扫描条目数与耗时；
- 远程路径 SMB / NFS / WebDAV 标识；
- PDF「显示全部」；
- 视频内置播放器、胶囊控制条、HLS 无感拖动；
- canvas 隐藏系统原生视频控件。

## 进程与资源

```text
systemd xuanlan.service
  -> /vol1/@appcenter/xuanlan/start.sh
  -> bin/xuanlan serve
```

单机单进程，无数据库；构建低优先级、短锁快照，构建期间 `/api/status` 保持亚毫秒响应。
共享约 1×；隔离约用户数×；混合 1× + 每个隔离单元 1×。

## 本轮测试

新增：

```text
python3 tests/multisearch_test.py bin/xuanlan
multisearch: OK
```

覆盖全部/任一命中、引号精确词组、大小写、顺序、空格/逗号分隔、总开关和设置回显。

完整回归：

```text
markdown / smoke / api / scanmode / route / unitbuild / epoch
unitdelta / group-intersection / units-admin / repair / multisearch / ui
```

全部通过。ext4 镜像测试未执行：本机 Docker 缺少 `subnetdesk-build:bookworm`；
脚本已支持 `XL_EXT4_IMAGE` 覆盖镜像。

## 兼容与边界

- 多词匹配目标是文件名，不是完整路径；
- 单次查询最多 512 字节、最多 32 个词；
- 少于 3 个字符的词走名字池扫描，速度较慢；
- 顺序匹配只对「同时命中」生效；
- 索引不加密、不含全文检索；单机单进程。
