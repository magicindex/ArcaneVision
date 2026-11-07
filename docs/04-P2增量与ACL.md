# P2 增量层与 TrimACL 求值

## 一、fanotify 实测结论（kernel 6.18.18.c1126-trim）

| 组合 | 结果 |
|---|---|
| `FAN_MARK_ADD\|FAN_MARK_MOUNT` + `FAN_REPORT_FID` | `EINVAL`（任何 mask 组合） |
| `FAN_MARK_FILESYSTEM` + `FAN_REPORT_FID` | 成功（ext4 / ZFS） |
| tmpfs 上 `fanotify_mark` | `EOPNOTSUPP` |
| 非 root 调用 `fanotify_init` | `EPERM` |

初始化必须带 `FAN_CLASS_NOTIF | FAN_CLOEXEC | FAN_REPORT_FID | FAN_REPORT_DFID_NAME`。
DFID_NAME 事件给出父目录的 `file_handle` 与子项名：

- 名字偏移 = `ALIGN(sizeof(struct file_handle) + handle_bytes, 4)`（本机实测 16）；
- 名字长度含结尾 NUL；
- `open_by_handle_at()` 的 mount fd 必须是 `O_RDONLY | O_DIRECTORY`（`O_PATH` 会 `EBADF`）；
- 还原出的路径需剥掉 `" (deleted)"` 后缀。

事件位实测：`0x40` MOVED_FROM、`0x80` MOVED_TO、`0x100` CREATE。

## 二、delta 层设计

```
reader 线程  poll/read → 解析 DFID_NAME → open_by_handle_at 还原父目录 → 入队
worker 线程  静默 250ms → 批量 apply → 更新 adds / dels → 唤醒查询
```

- **adds**：`CREATE` / `MOVED_TO` 后 `lstat`，要求 `st_dev` 与卷设备一致（嵌套挂载内容不入索引）。
- **dels**：`DELETE` / `MOVED_FROM` 记墓碑；同一路径再次出现时清除墓碑。
- 两个集合都用开放寻址哈希表（FNV-1a 64 位），删除用墓碑标记，避免线性探测删除重排。
- 上限 131,072 条，超出置 `fatal`，由对账线程整卷重建（`live_overflowed()`）。
- 查询时把 delta 与基线按 mtime 归并；输出前用 `live_is_deleted()` 跳过墓碑。
- 存在 delta 时，`total` 可能多算被删除项，响应里带 `"approx": true` 标记。

对账：默认每 6 小时（`--rebuild-hours`）重建一次；重建开始时 `live_clear()` 清空 delta。

## 三、TrimACL 二进制格式（实测）

xattr 名 `system.trim_acl`，无该 xattr 时回落 POSIX mode。

```
u32 version = 1
重复 N 次（每 20 字节）:
    u32 type_tag
    u32 perms
    u32 inherit
    u32 id
    u32 reserved
```

**type_tag**：`0x01` allow、`0x02` deny；标签位 `0x04` user、`0x08` group、`0x10` everyone、`0x20` owner。
实测样本：`0x05` = allow+user，`0x0a` = deny+group，`0x11` = allow+everyone，`0x21` = allow+owner。

**perms**（用 `trimacl -m a:u:1000:<字母>:sfd` 逐个实测）：

| 字母 | 值 | 字母 | 值 |
|---|---|---|---|
| x | `0x0001` | e | `0x0040` |
| w | `0x0002` | c | `0x0080` |
| r | `0x0004` | a | `0x0100` |
| p | `0x0008` | A | `0x0200` |
| D | `0x2000` | E | `0x0400` |

例：`rwxpDaAeEc` = `0x27CF`，`rx` = `0x05`，`rwx` = `0x07`。

**inherit**：`0x1` self、`0x2` file、`0x4` dir；`0x4000` 是 `<exclude inheritance>` 边界标记。

**id**：uid / gid；owner 与 everyone 用 `0xffffffff`。

样本：`/vol1/1000` = `01 00 00 00 | 05 00 00 00 05 00 00 00 07 00 00 00 84 03 00 00 00 00 00 00`
（allow uid:900 rx inherit self,file,dir；`0x384` = 900）。

## 四、求值语义

1. 目标目录自身有 ACL → 用自身条目求值（不带 inherit 的条目只影响本目录）。
2. 自身没有 → 逐级向上找最近带 ACL 的祖先，只采纳 `inherit & 0x4`（dir）的条目。
3. 都没有 → 回落 POSIX mode 的读位。
4. deny 覆盖 allow（同一权限位）。
5. **列目录需要 `r` 位**（`0x0004`），不满足即隐藏该目录下所有条目。

实测（容器内 root）：

| uid | /vol1/1000 | /vol1/1000/AI | /vol1 | /vol1/vm |
|---|---|---|---|---|
| 900 | 1 | 1（继承） | 0 | 1 |
| 878 | 0 | — | 0 | 1（everyone rxaec） |

## 五、身份来源（未决）

fnOS 网关（`/usr/trim/bin/trim_open_gateway`）字符串里只有 `X-Forwarded-For` / `X-Real-IP`，
没有用户身份头。应用侧如何拿到当前用户尚未确认，因此：

- 服务默认 **不过滤**（`g_acl_uid = -1`），适合单管理员使用；
- 设 `XL_ACL_UID=<uid>` 时按该 uid 过滤（联调/测试用）；
- 真机安装后需要确认网关或系统 API 的用户标识机制，再替换成按请求取 uid。

## 六、验证记录

容器 `xl-live2`（`--privileged --network=host`，rbind ext4 测试树）：

| 动作 | 结果 |
|---|---|
| 新增 `added.md`、`新建文件.md` | 立即命中，`adds=2` |
| 删除 `alpha.txt` | 查询 items 为空 |
| `mv beta.log gamma.log` | beta 消失、gamma 命中，`dels=2` |
| 结果 size/mtime | 输出前 `stat` 前 N 条，真实值 |

## 请求级身份（0.1.4）

网关（`trim_open_gateway`）调用应用时用 `x-nas-uid` 请求头带上已认证用户的 uid。索引服务在
`handle_client()` 里解析该头（大小写不敏感、只认行首），有值就按该 uid 做 ACL 判定，
没有再回落到启动时的 `XL_ACL_UID`；两者都没有则不裁剪（当前 `allUsers=false`，只有管理员能打开页面）。

判定仍在输出阶段逐条做：取结果的父目录路径交给 `perm_dir_listable()`，不可见就跳过。
`total` 仍是未裁剪的候选数，`approx` 字段标记这一点。

实测（`/vol1` 全量索引，`q=IMG_`，limit 500）：

| 请求身份 | `/vol1/1000` 下命中 | `/vol2/1000` 下命中 |
|---|---|---|
| `x-nas-uid: 878` | 0 | 1 |
| `x-nas-uid: 900` | 500 | 0 |

说明过滤按用户生效，而不是全局开关。

## 单实例与托管

- 启动时先探 `app.sock`：能连上且 `/healthz` 返回 200 就直接退出，避免 App Center 与 systemd 各拉一个进程抢索引目录。
- 安装目录外另置 `xuanlan.service`（`/etc/systemd/system/xuanlan.service`），`Restart=always` + `enable`，重启 NAS 后自动拉起；App Center 再启动时会被上面的探测挡下。

## 0.1.7 修正（身份与权限）

**身份来源**：飞牛网关经 `trim_http_cgi` 转发时注入 `X-Trim-Userid` / `X-Trim-Username` /
`X-Trim-Isadmin`（不用 Cookie，应用无需也无法自行鉴权）。诊断办法：首次请求打印头名（不打值）。
`/api/me` 的 `via` 字段用于确认来源：`trim` 表示命中飞牛头。

**权限求值三处修正**：

| 问题 | 现象 | 修正 |
| --- | --- | --- |
| 只看目标目录 | 深层文件绕过 `/vol1/docker` 的拒绝 | 路径自下而上逐级判「可穿过 x」，目标目录另判「可读 r」 |
| 用进程自身属组 | root 进程把 `root:root 0710` 判成组可读 | 用 `getgrouplist(目标 uid)` 解析属组并缓存 |
| 卷根 mode 0 | `/vol1` 为 `d---------`，逐级判定会全盘否决 | `/volN` 卷根不作为穿越条件（飞牛内核特例，实测可穿越） |

**系统目录**：`@appmeta` / `@sysappmeta` / `@appcenter` / `@apphome` / `@appdata` / `@appconf`
POSIX 755 但对普通用户无意义，查询期对非管理员直接跳过。

验证（`xuanlan acl <uid> <dir>`，root 下执行）：

```
uid 900 /vol1/1000                    -> 1
uid 1000 /vol1/1000                   -> 0
uid 878 /vol1/1000/NextCloud          -> 0（祖先拒绝）
uid 1000 /vol1/docker/overlay2/…/diff -> 0（0.1.6 为 1，越权）
```

HTTP 层（`X-Trim-Userid` 取 1000 / 900 / 878，`q=IMG_`）过滤后的归属分布与各自 ACL 一致。

## 0.8.6 修正（Administrators 组）

实测发现内核还允许 **`Administrators` 组（gid 1000）跨过 TrimACL 读取整卷**，与 root
类似；这一点此前没有在 `perm.c` 里跟随，导致管理员搜不到自己实际能打开的文件。

隔离复现（容器里用 `--user UID:GID` 只给单一组，避免继承）：

| 进程 uid:gid | `/vol1/@appdata/xuanlan`（root 700） | `/vol1/1001`（1001 700） | NextCloud `data/admin/files` |
| --- | --- | --- | --- |
| 500:1000 | 可读 | 可读 | 可读 |
| 1000:1000 | 可读 | 可读 | 可读 |
| 1000:1001 | 拒绝 | 拒绝 | 可读（家目录属主/projid=1000） |
| 1001:1001 | 拒绝 | 可读 | 拒绝 |

结论：判断依据是进程是否带 **gid 1000**；换成 `1001:1000` 同样全通。`www-data`
（uid 712）另有系统账号特权，与普通用户无关。

修正方式：`perm_check()` / `perm_mode_readable()` 在 uid 0 之外，若目标 uid 属于
`getgrnam("Administrators")` 的组，也直接放行。非管理员仍走原有 ACE/POSIX 求值。

验证：

```
xuanlan acl 1000 /vol1/1000/NextCloud/data/admin/files   -> 1
xuanlan acl 1001 /vol1/1000/NextCloud/data/admin/files   -> 0
xuanlan acl 12345 /vol1/1000/NextCloud/data/admin/files  -> 0
```

unix socket 上带 `x-trim-userid: 1000` 查询 `工程施工派工单` 返回 1 条；`1001` 仍为 0 条。

