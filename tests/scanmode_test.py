#!/usr/bin/env python3
"""扫描模式解析/布局回归（P1）。"""
import json
import os
import stat
import subprocess
import sys
import tempfile

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
TMP = tempfile.mkdtemp(prefix="xl-scanmode-")

def run(args):
    r = subprocess.run([BIN] + args, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL run", args, r.returncode, r.stderr.strip())
        sys.exit(1)
    return r.stdout

def cfg(name, mode, share_groups=(), isolated_groups=(), share_users=(), isolated_users=()):
    p = os.path.join(TMP, name)
    with open(p, "w", encoding="utf-8") as f:
        json.dump({
            "version": 2, "scanMode": mode,
            "shareGroups": list(share_groups),
            "isolatedGroups": list(isolated_groups),
            "shareUsers": list(share_users),
            "isolatedUsers": list(isolated_users),
        }, f)
    return p

def units(cfg_path, uid, gids=""):
    args = ["scanmode", "--config", cfg_path, "--uid", str(uid), "--json"]
    if gids:
        args += ["--gids", gids]
    return json.loads(run(args))["units"]

def expect(name, got, want):
    if got != want:
        print(f"FAIL {name}: got {got!r}, want {want!r}")
        sys.exit(1)

def keys(us):
    return [u["unit"] for u in us]

# C1 默认/缺失配置 -> shared
expect("C1 default", keys(units(os.path.join(TMP, "missing.json"), 1000)), ["shared"])

# C2 shared 忽略分类
p = cfg("shared.json", "shared", isolated_groups=[9001], share_groups=[9002],
        isolated_users=[1000], share_users=[1001])
expect("C2 shared ignores", keys(units(p, 1000, "9001,9002")), ["shared"])

# C3 isolated -> 每人 u<uid>
p = cfg("isolated.json", "isolated", isolated_groups=[9001])
expect("C3 isolated", keys(units(p, 1000, "9001")), ["u1000"])
expect("C3 isolated other", keys(units(p, 1001)), ["u1001"])

# C4 hybrid 未分类 -> shared
p = cfg("h0.json", "hybrid")
expect("C4 fallback", keys(units(p, 1000)), ["shared"])
expect("C4 fallback flag", units(p, 1000, "")[0]["fallback"], True)

# C5 isolated group -> g<gid>
p = cfg("h1.json", "hybrid", isolated_groups=[9001])
expect("C5 group", keys(units(p, 1000, "9001")), ["g9001"])
expect("C5 group kind", units(p, 1000, "9001")[0]["kind"], "group")

# C6 share group -> shared
p = cfg("h2.json", "hybrid", share_groups=[9002])
expect("C6 share group", keys(units(p, 1000, "9002")), ["shared"])

# C7 用户覆盖 isolated
p = cfg("h3.json", "hybrid", isolated_users=[1000], isolated_groups=[9001])
expect("C7 user isolated", keys(units(p, 1000, "9001")), ["u1000"])

# C8 用户覆盖 share 优先于 isolated 组
p = cfg("h4.json", "hybrid", share_users=[1000], isolated_groups=[9001])
expect("C8 user share", keys(units(p, 1000, "9001")), ["shared"])

# C9 同属 isolated + share 组 -> 隔离优先
p = cfg("h5.json", "hybrid", isolated_groups=[9001], share_groups=[9002])
expect("C9 isolated wins", keys(units(p, 1000, "9001,9002")), ["g9001"])

# C10 多个 isolated 组 -> 多单元，升序
p = cfg("h6.json", "hybrid", isolated_groups=[9002, 9001])
expect("C10 multi", keys(units(p, 1000, "9002,9001")), ["g9001", "g9002"])
expect("C10 multi count", len(units(p, 1000, "9002,9001")), 2)

# C11 未知 gid 忽略
p = cfg("h7.json", "hybrid", isolated_groups=[9001])
expect("C11 unknown", keys(units(p, 1000, "7777")), ["shared"])

# C12 损坏配置 -> shared
p = os.path.join(TMP, "bad.json")
open(p, "w").write('{"scanMode":"bogus","isolatedGroups":[9001]}')
expect("C12 bad config", keys(units(p, 1000, "9001")), ["shared"])

# C13 units init 创建 0700 目录且不碰共享索引
idx = os.path.join(TMP, "idx")
os.makedirs(idx)
out = run(["units", "init", "--index-dir", idx])
up = os.path.join(idx, "units")
expect("C13 output", "units:" in out, True)
expect("C13 exists", os.path.isdir(up), True)
expect("C13 mode", stat.S_IMODE(os.stat(up).st_mode), 0o700)
expect("C13 no shared touched", sorted(os.listdir(idx)), ["units"])

print("scanmode: OK")
