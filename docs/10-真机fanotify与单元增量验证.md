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
