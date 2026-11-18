# 真机 fanotify → 队列 → 批量 delta → 重启恢复验证

> 日期：2026-11-18 · 环境：飞牛 fnOS · 二进制：0.9.0 + 0.10 开发补丁
> 结论：**通过**。使用独立临时实例，未改动生产共享索引。

## 1. 测试隔离

为避免在生产 200 万条索引上等待组单元构建，使用同一台真机、同一个二进制启动临时实例：

```text
根目录：/vol1/@team/xl-e2e
配置：/tmp/xl-e2e/scanmode.json
索引：/tmp/xl-e2e/idx
socket：/tmp/xl-e2e/app.sock
模式：hybrid + isolatedGroups=[1001]
组成员：uid 1000 / 1001 / 1002（主组 1001）
```

服务以 root 启动，fanotify 正常启用；生产服务 `xuanlan.service` 不受影响。

## 2. 单元构建

```text
xuanlan serve: fanotify 实时增量已启用
xuanlan: 开始构建 1 个隔离单元
xuanlan: unit g1001 完成（1 卷）
```

`/api/status`：

```json
{"state":"ready","records":2,"units":{"count":1},"unitBuild":null}
```

单元文件：

```text
/tmp/xl-e2e/idx/units/g1001/xl-e2e.idx
/tmp/xl-e2e/idx/units/g1001/unit.json
```

查询 `initial`：命中 `initial.txt`，说明组单元路由 + 查询期 ACL 正常。

## 3. fanotify 新增事件

创建：

```text
/vol1/@team/xl-e2e/fanotify-new.txt
```

2 秒后：

- `/api/status.live.adds = 1`
- `events.queued = 0`、`events.dropped = 0`
- `units/g1001/delta.log` 出现，大小 55 字节，包含 `fanotify-new.txt`
- 查询 `fanotify` 命中该文件

这验证了：

```text
fanotify → unit_event_cb 入队 → unit_writer 批量写 → delta.log → 查询合并
```

## 4. 重启恢复

重启临时服务后：

- 查询 `fanotify` 仍命中 `fanotify-new.txt`；
- `delta.log` 保留，未被重建清空；
- 单元快照未重建，结果来自快照 + delta 合并。

## 5. 删除与墓碑

删除 `fanotify-new.txt` 后查询为 0；重启后仍为 0，说明删除事件已持久化为墓碑。

## 6. 批量事件

一次创建 5 个文件：

```text
batch-1.txt ... batch-5.txt
```

2 秒后：

- 查询命中 5 个文件；
- `delta.log` 610 字节，包含全部 5 个路径；
- 重启后仍命中 5 个文件。

说明一次 writer 批次可以写入多条事件，队列没有丢弃。

## 7. 边界与说明

- 本次使用独立临时实例，避免在生产索引上触发 200 万条组单元扫描；生产服务保持 `scanMode=shared`。
- 测试后已删除 `/vol1/@team/xl-e2e`、`/tmp/xl-e2e` 和临时容器。
- `total` 在 live 与单元结果合并时可能偏大（近似语义），但 `items` 去重正确。
- 官方应用中心 FPK 安装回归仍待完成；本次使用已安装应用的二进制启动临时实例。
- 单元构建进度可通过 `/api/status.unitBuild` 观察；组单元 ACL 批量读取优化已生效。

## 8. FPK 应用中心安装回归（2026-11-18）

在同机执行实体 FPK 应用中心安装回归，结果：**未通过，阻塞在 App Center 安装任务**。

### 8.1 已确认正常

```text
fnos-cli app install-fpk xuanlan_0.9.0_amd64.fpk --volume-id 1 --dry-run --yes
```

- FPK 上传、解析、向导参数、卷约束、安装信息全部通过；
- 识别到已安装版本、`canUpgrade=true`、目标版本 0.9.0；
- 真实安装任务返回：

```json
{"code":10236,"data":null,"msg":""}
```

- 停止 `xuanlan.service`、释放 8971 端口后重试，仍返回 10236；
- 清理 App Center 中 xuanlan 记录、移动 `/var/apps/xuanlan` 后重试，仍返回 10236；
- 换 0.9.1 包、`--no-start`、不同卷/数据卷组合，均返回 10236；
- 说明：FPK 本身解析正常，问题在 App Center 当前安装状态/安装任务，而非包格式。

### 8.2 现场恢复

- 已恢复 `/vol1/@appcenter/xuanlan` 与 `/var/apps/xuanlan`；
- 已从备份恢复 App Center DB 中的 xuanlan 记录；
- `xuanlan.service` 已恢复运行：`ready`、`scanMode=shared`、`units.count=0`；
- 应用数据 `/vol1/@appdata/xuanlan` 未删除；
- 备份：
  - `/vol1/@appcenter/xuanlan.bak-20261118-224039`
  - `/vol1/@appshare/xuanlan-appdata-backup-20261118-232118`
  - `/vol1/@appshare/appcenter-backup-20261118-232641.sql`

### 8.3 后续

- 需在 App Center UI 中做一次“卸载/重装”或清理异常安装任务后再验证；
- 在 App Center 安装任务恢复前，0.9.0 继续以已有安装 + 手动 FPK payload 升级方式运行；
- 该问题不改变 FPK 内容；应用中心安装回归仍为 0.10 待办。

## 9. FPK 应用中心安装回归（2026-11-19 复测通过）

前一节 10236 的真实原因是：`fnos-cli` profile 实际连接的是 **FnOSF6 / 192.168.3.202**，
而不是当前机器 **FnOSF10 / 192.168.3.201**。之前所有 App Center 安装任务都发到了错误主机。

### 9.1 切换本机 profile

新建并通过设备授权登录 `fnosf10`：

```text
host     = 192.168.3.201
hostName = FnOSF10
uid      = 1000
admin    = true
```

验证：

```bash
fnos-cli --profile fnosf10 system host-name
# FnOSF10
```

### 9.2 清理异常安装状态

- 停止本机 `xuanlan.service`；
- 删除本机 App Center 中 xuanlan 记录及关联行；
- 移走旧 `/var/apps/xuanlan` 与 `/vol1/@appcenter/xuanlan`；
- 删除旧 systemd unit；
- 保留 `/vol1/@appdata/xuanlan` 索引数据；
- 清理残留应用用户/组，避免 `APP_INSTALL_FAILED_USER_ALREADY_EXISTS`。

### 9.3 安装结果

```bash
fnos-cli --profile fnosf10 app install-fpk \
  xuanlan_0.9.0_amd64.fpk \
  --volume-id 1 \
  --custom-parameters '[{"key":"wizard_service_port","value":"8971"}]' \
  --yes
```

返回：

```text
Started install task ... for xuanlan 0.9.0
```

状态轮询：

```text
starting → running
```

最终：

```text
appName = xuanlan
version = 0.9.0
status  = running
服务    = active
```

FPK manifest：

```text
version  = 0.9.0
checksum = e012cd737c3d3a578c13ce75142fd70c
```

### 9.4 安装后验证

- `/api/status`：`ready`、`version=0.9.0`、`scanMode=shared`；
- `/api/scanmode/candidates`：正常返回 200；
- `/vol1/@appdata/xuanlan/settings.json`、`scanmode.json` 保留；
- 索引数据未清空；
- App Center 显示 0.9.0 / running。

结论：**FnOSF10 本机 FPK 应用中心安装通过**。

## 10. ACL 变更与压力测试（2026-11-19）

### 10.1 实现

- `live.c` fanotify mask 增加 `FAN_ATTRIB`；
- 文件/子目录 chmod、chown、xattr 变化标记 `acl_dirty`；
- `/api/status.live` 增加 `aclEvents` / `aclDirty`；
- 主循环静默 5 秒后只触发一次重建，避免批量 ACL 变化造成重建风暴；
- 新建文件可能携带 `FAN_ATTRIB`，已排除 `CREATE/DELETE/MOVE/MODIFY/CLOSE_WRITE` 组合事件；
- 单元事件队列从 4096 提升到 16384；
- 队列溢出时记录 dropped 并触发一次重建。

### 10.2 ACL 变更验证

测试根：`/vol1/@team/xl-stress`，hybrid + `isolatedGroups=[1001]`。

单文件 `chmod 600`：

```text
aclEvents = 1
aclDirty  = true
5 秒后：检测到 ACL/属性变化，触发重建
```

重建后该文件从组单元查询中消失，符合 ACL 预期。

批量 `chmod 640 stress-*.txt`（5000 个）：

```text
aclEvents = 5000
重建日志   = 1 次
events.queued = 0
events.dropped = 0
```

验证了批量 ACL 事件合并为一次重建。

### 10.3 压力测试

5000 个文件快速创建：

```text
live.adds      = 5000
events.queued  = 0（最终）
events.dropped = 0
delta.log      = 857,127 字节
```

单元 writer 队列峰值约 5553 条，最终全部排空，无丢弃。

> 测试使用独立临时实例，生产 `xuanlan.service` 未受影响；测试后已删除
> `/vol1/@team/xl-stress`、`/tmp/xl-stress` 和临时容器。


## 11. 0.10.0 FPK 安装与发布

- 构建：`xuanlan_0.10.0_amd64.fpk`（1,026,888 字节，SHA-256
  `53daa3004be390964e0e09f3f920bbc3cfe3d2624e92523b2bcede8d3a1a4da4`）；
- 在 FnOSF10 本机通过 `fnosf10` profile 完成 App Center 安装；
- 安装后版本 0.10.0、服务 running、候选 API 正常；
- `/vol1/@appdata/xuanlan/settings.json`、`scanmode.json` 保留；
- Gitea / GitHub 发布 `v0.10.0`（prerelease），附 FPK。

## 12. 容量/修复趋势（0.10.x）

- 服务每 60 秒记录一次单元容量、修复失败累计值；
- 保留最近 60 个采样；
- `/api/status.trend.capacity` / `.repairFailed` 输出数组；
- 管理端状态行在采样 ≥2 次时显示容量趋势条，并标注最近修复失败数；
- 采样与读取通过 mutex 保护。

说明：趋势仅保留当前进程生命周期，重启后重新累计。

## 13. 快照 checksum（0.10.x）

- `xl_hdr.pad0` 改为 `checksum`（128B 布局不变）；
- `snapshot_save()` 写入完成后对全文件计算 FNV-1a，回写 checksum 并 fsync；
- `snapshot_open()` 对 `checksum != 0` 的新快照重新计算并比对；
- 旧快照 checksum=0 视为兼容，不做全文件校验；
- 损坏测试：翻转索引尾部 1 字节，`info` 立即报
  `index checksum mismatch` 并拒绝打开。

代价：打开新快照时多一次全文件读取；旧快照不受影响。


## 14. 子树级 ACL 增量重建（0.10.1）

实现：

- fanotify `FAN_ATTRIB` 事件记录变化路径队列（最多 64 条）；
- 静默 5 秒后对每个活跃单元执行 `xl_unit_patch_to_file()`；
- 旧单元快照中保留变化子树之外的记录，只重扫变化目录；
- 新子树记录合并、重建倒排索引、原子替换；
- 更新 `unit.json` mtime，使单元缓存 epoch 切换；
- 若子树合并失败，回退该单元全量重建；
- 共享索引不再因 ACL 变化重建。

真机验证：

```text
初始 secret.txt mode 600 → 组单元查询不到
chmod 644 → FAN_ATTRIB → 子树增量更新 secret.txt -> g1001
查询命中 secret.txt
chmod 600 → 子树增量更新 → 查询再次为空
```

日志确认只出现“ACL 子树增量更新”，没有整卷重建。


## 15. 远程挂载 provider MVP（0.10.1）

- 检测 `/vol02/` 下 `cifs` / `smb3` / `nfs` / `nfs4` / `davfs` 挂载；
- `GET /api/remote` 返回挂载路径、类型、owner uid、索引状态、记录数；未启用索引时也列出已挂载 CIFS（实测 2 个 /vol02 CIFS）；
- 设置 `XL_REMOTE_INDEX=1` 时加入共享索引，索引文件 `idx/remote-<mount>.idx`；
- 权限：从 `/vol02/<uid>-...` 解析 owner，只有 owner 或管理员可检索；不进入隔离单元；
- 默认不开启远程索引，避免自动扫描大容量 CIFS 库；需要显式开启；
- 远程健康检查每 60 秒；不可达超过 TTL（默认 300 秒）后查询隐藏；
- 管理端设置页增加远程挂载卡片，展示挂载/索引/离线状态；
- 远程索引每 `XL_REMOTE_SYNC_SEC`（默认 900 秒）对账重建，仅重建远程卷；
- 对账线程与全量构建互斥，`/api/status.remoteSync` 暴露 running/last；
- 生命周期：对账时按 `mnt_fsname` 识别同一共享，换挂载点保留索引并标记重建；掉线/卸载删除对应 `remote-*.idx` 并移出卷数组。


## 16. 远程 ACL 适配器（0.10.1）

- CIFS/SMB3：adapter `cifs`；当前挂载 `forceuid/0777` 且未启用 `cifsacl`，无法读取服务端 ACL，
  安全回退 owner-only，并在 `/api/remote.aclReason` 说明；
- NFS/NFS4：有 `nfs4_getfacl` 时 adapter `nfs4`，否则 `posix`，按 POSIX mode 判断；
- WebDAV：adapter `owner`/`posix`；
- 显式映射：`/vol1/@appconf/xuanlan/remote_acl.json`
  ```json
  {"mounts":[{"root":"/vol02/1000-0-aea4b1f9","users":[1000,1001]}]}
  ```
  adapter 变为 `config`，只有 users 列表 + owner/管理员可见；
- 真机临时验证：CIFS 挂载配置 users=[1000,1001]，uid1001 命中、uid1002 为空。


## 17. 远程索引设置页开关/立即重建（0.10.1）

- 设置页远程卡片提供“启用/关闭”和“立即重建”；
- `POST /api/remote {"enabled":true|false}` 写 `remote.json` 并即时增删远程卷；
- `POST /api/remote/rebuild` 立即启动远程对账；
- 关闭时删除远程卷与 `remote-*.idx`，开启时重新探测并构建；
- 构建/对账进行中拒绝重复操作。


## 18. CIFS 服务端 ACL 解析尝试（0.10.2）

- 已对两个 CIFS 挂载执行 `mount -o remount,cifsacl`，`/proc/mounts` 已包含 `cifsacl`；
- 实现 CIFS ACE 解析：`getcifsacl -r` → OWNER/ACL 行 → 本地用户名/组映射 → allow/deny；
- 60 秒负缓存，避免 ACL 不可用时每个结果 fork；
- 当前内核 6.18.18 下 `getcifsacl` 对两个共享均返回 `Operation not permitted`，
  因此实际仍回退 owner-only；`/api/remote.aclReason` 可观测；
- 结论：代码路径已具备，但服务端/内核未返回 security descriptor，需在支持 cifsacl 的挂载/内核环境验证。
