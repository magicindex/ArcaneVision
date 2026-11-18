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
