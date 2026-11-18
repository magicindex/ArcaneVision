#!/usr/bin/env python3
"""P9：损坏的单元索引被检测并自动重建。"""
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
TMP = tempfile.mkdtemp(prefix="xl-repair-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol1")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX); os.makedirs(VOL)
uid = os.getuid()
if uid == 0:
    uid = sorted(u.pw_uid for u in pwd.getpwall() if u.pw_uid >= 1000)[0]
open(os.path.join(VOL, "repair.txt"), "w").write("x")
open(os.path.join(TMP, "scope.json"), "w").write(json.dumps({
    "version": 2, "scanMode": "hybrid", "shareGroups": [], "isolatedGroups": [],
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
    unit = os.path.join(IDX, "units", "u%d" % uid)
    for _ in range(200):
        if os.path.exists(os.path.join(unit, "unit.json")): break
        time.sleep(0.05)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

# 截断单元索引，模拟损坏
open(os.path.join(unit, "vol1.idx"), "wb").write(b"BADIDX")
p = start()
try:
    # 首次查询触发检测 + 入队重建；轮询直到恢复
    ok = False
    for _ in range(200):
        if count("repair") == 1:
            ok = True; break
        time.sleep(0.05)
    if not ok:
        print("FAIL repair not completed"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

print("repair: OK")
