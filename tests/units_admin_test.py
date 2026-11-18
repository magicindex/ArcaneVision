#!/usr/bin/env python3
"""P7：delta 压缩、容量统计、旧单元 GC、回滚。"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
TMP = tempfile.mkdtemp(prefix="xl-uadmin-")
IDX = os.path.join(TMP, "idx")
os.makedirs(os.path.join(IDX, "units"))

def run(args):
    r = subprocess.run([BIN] + args, capture_output=True, text=True)
    if r.returncode != 0:
        print("FAIL run", args, r.stderr); sys.exit(1)
    return r.stdout

def mkunit(key, age_days=0):
    d = os.path.join(IDX, "units", key)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "vol1.idx"), "w").write("x" * 4096)
    open(os.path.join(d, "unit.json"), "w").write("{}")
    t = time.time() - age_days * 86400
    os.utime(d, (t, t))

def rec(path, deleted=False, size=0):
    return struct.pack("<IIBBHII", 0x31444c58, len(path), 1 if deleted else 0, 0, 0, size, 0) + path.encode()

mkunit("u1000")
mkunit("g9001", 10)
mkunit("u2000", 10)

# 生成超过阈值的重复 delta，compact 后应显著变小
dlog = os.path.join(IDX, "units", "u1000", "delta.log")
with open(dlog, "wb") as f:
    for i in range(15000):
        f.write(rec("/vol1/a.txt", False, i))
        f.write(rec("/vol1/a.txt", True))
        f.write(rec("/vol1/b.txt", False, i))
before = os.path.getsize(dlog)
out = run(["units", "compact", "--index-dir", IDX, "--threshold-mb", "1"])
after = os.path.getsize(dlog)
if after >= before:
    print("FAIL compact", before, after); sys.exit(1)

out = run(["units", "stats", "--index-dir", IDX])
if "units=3" not in out:
    print("FAIL stats", out); sys.exit(1)

out = run(["units", "gc", "--index-dir", IDX, "--active", "u1000", "--grace-days", "0", "--purge"])
if not os.path.isdir(os.path.join(IDX, "units", "u1000")):
    print("FAIL gc removed active"); sys.exit(1)
for gone in ("g9001", "u2000"):
    if os.path.exists(os.path.join(IDX, "units", gone)):
        print("FAIL gc kept", gone); sys.exit(1)

out = run(["units", "rollback", "--index-dir", IDX])
sm = json.load(open(os.path.join(TMP, "scanmode.json")))
if sm.get("scanMode") != "shared":
    print("FAIL rollback", sm); sys.exit(1)

print("units-admin: OK")
