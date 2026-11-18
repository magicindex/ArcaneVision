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
