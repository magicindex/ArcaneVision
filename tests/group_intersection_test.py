#!/usr/bin/env python3
"""P6：组单元按成员精确交集生成（非并集）。"""
import grp
import json
import os
import pwd
import socket
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="xl-gint-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol1")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX); os.makedirs(VOL)
uid = os.getuid()
if uid == 0:
    uid = sorted(u.pw_uid for u in pwd.getpwall() if u.pw_uid >= 1000)[0]
open(os.path.join(VOL, "gonly.txt"), "w").write("g")

# 找一个包含当前用户、且有其他成员的组；其他人无法读 0700 临时目录
gid = None
me = pwd.getpwuid(uid).pw_name
for g in grp.getgrall():
    if me in g.gr_mem and any(m != me for m in g.gr_mem):
        gid = g.gr_gid; break
if gid is None:
    print("group-intersection: SKIP (no multi-member group)")
    sys.exit(0)

open(os.path.join(TMP, "scope.json"), "w").write(json.dumps({
    "version": 2, "scanMode": "hybrid", "shareGroups": [],
    "isolatedGroups": [gid], "shareUsers": [], "isolatedUsers": [],
}))

def start():
    p = subprocess.Popen([BIN, "serve", "--no-tcp", "--socket", SOCK, "--ui", "ui",
                          "--index-dir", IDX, "--roots", VOL],
                         cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(SOCK): return p
        if p.poll() is not None: raise RuntimeError("serve exited")
        time.sleep(0.05)
    p.kill(); raise RuntimeError("socket timeout")

def count(term):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall(("GET /api/search?q=%s&limit=50 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
               "x-nas-uid: %d\r\n\r\n" % (term, uid)).encode())
    d = b""
    while True:
        x = s.recv(65536)
        if not x: break
        d += x
    s.close()
    return len(json.loads(d.partition(b"\r\n\r\n")[2]).get("items", []))

p = start()
try:
    unit = os.path.join(IDX, "units", "g%d" % gid)
    for _ in range(200):
        if os.path.exists(os.path.join(unit, "unit.json")): break
        time.sleep(0.05)
    if not os.path.exists(os.path.join(unit, "unit.json")):
        print("FAIL group unit missing"); sys.exit(1)
    if count("gonly") != 0:
        print("FAIL group intersection: non-member-visible file leaked"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

print("group-intersection: OK")
