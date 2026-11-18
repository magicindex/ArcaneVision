#!/usr/bin/env python3
"""QueryRouter 回归：hybrid 下 isolated 用户只读自己的单元索引。"""
import json
import os
import pwd
import shutil
import socket
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="xl-route-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol1")
UNIT = os.path.join(TMP, "unitroot")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX); os.makedirs(VOL); os.makedirs(UNIT)

uid = os.getuid()  # 用当前用户，确保临时目录 ACL 可通过
if uid == 0:
    uid = sorted(u.pw_uid for u in pwd.getpwall() if u.pw_uid >= 1000)[0]
open(os.path.join(VOL, "sharedonly.txt"), "w").write("shared")
open(os.path.join(UNIT, "unitonly.txt"), "w").write("unit")

# 共享索引 + 一个隔离用户单元
subprocess.run([BIN, "build", "--root", VOL, "--out", os.path.join(IDX, "vol1.idx")],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
unit_dir = os.path.join(IDX, "units", "u%d" % uid)
os.makedirs(unit_dir)
subprocess.run([BIN, "build", "--root", UNIT, "--out", os.path.join(unit_dir, "vol1.idx")],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# scope.json 在 idx 的父目录；hybrid + 显式隔离该 uid
open(os.path.join(TMP, "scope.json"), "w").write(json.dumps({
    "version": 2, "scanMode": "hybrid",
    "shareGroups": [], "isolatedGroups": [],
    "shareUsers": [], "isolatedUsers": [uid],
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

def req(path):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    h = ("GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
         "x-nas-uid: %d\r\n\r\n" % uid)
    s.sendall(h.encode())
    data = b""
    while True:
        d = s.recv(65536)
        if not d: break
        data += d
    s.close()
    body = data.partition(b"\r\n\r\n")[2]
    return json.loads(body)

def count(term):
    d = req("/api/search?q=" + term + "&limit=50")
    return len(d.get("items", []))

p = start()
try:
    if count("unitonly") != 1:
        print("FAIL unit route: unitonly not found"); sys.exit(1)
    if count("sharedonly") != 0:
        print("FAIL unit route: shared index leaked"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

# shared 模式仍读共享索引
open(os.path.join(TMP, "scope.json"), "w").write(json.dumps({
    "version": 2, "scanMode": "shared",
    "shareGroups": [], "isolatedGroups": [],
    "shareUsers": [], "isolatedUsers": [uid],
}))
p = start()
try:
    if count("sharedonly") != 1:
        print("FAIL shared route: sharedonly not found"); sys.exit(1)
    if count("unitonly") != 0:
        print("FAIL shared route: unit leaked"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

# hybrid 组模式：当前用户主组命中 isolatedGroups，读 g<gid> 单元
gid = os.getgid()
gdir = os.path.join(IDX, "units", "g%d" % gid)
os.makedirs(gdir)
subprocess.run([BIN, "build", "--root", UNIT, "--out", os.path.join(gdir, "vol1.idx")],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
open(os.path.join(TMP, "scope.json"), "w").write(json.dumps({
    "version": 2, "scanMode": "hybrid",
    "shareGroups": [], "isolatedGroups": [gid],
    "shareUsers": [], "isolatedUsers": [],
}))
p = start()
try:
    if count("unitonly") != 1:
        print("FAIL group route: unitonly not found"); sys.exit(1)
    if count("sharedonly") != 0:
        print("FAIL group route: shared leaked"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

# 隔离单元缺失：fail-closed，绝不回退共享
open(os.path.join(TMP, "scope.json"), "w").write(json.dumps({
    "version": 2, "scanMode": "hybrid",
    "shareGroups": [], "isolatedGroups": [],
    "shareUsers": [], "isolatedUsers": [uid],
}))
shutil.rmtree(unit_dir)
p = start()
try:
    if count("sharedonly") != 0:
        print("FAIL missing unit: fell back to shared"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=3)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

print("route: OK")
