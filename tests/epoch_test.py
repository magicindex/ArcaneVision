#!/usr/bin/env python3
"""P4：单元索引 epoch 原子切换（新请求加载新快照，旧快照不崩溃）。"""
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
TMP = tempfile.mkdtemp(prefix="xl-epoch-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol1")
NEW = os.path.join(TMP, "newroot")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX); os.makedirs(VOL); os.makedirs(NEW)
uid = os.getuid()
if uid == 0:
    uid = sorted(u.pw_uid for u in pwd.getpwall() if u.pw_uid >= 1000)[0]
open(os.path.join(VOL, "old.txt"), "w").write("old")
open(os.path.join(NEW, "new.txt"), "w").write("new")

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

def req(path):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall(("GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
               "x-nas-uid: %d\r\n\r\n" % uid).encode())
    data = b""
    while True:
        d = s.recv(65536)
        if not d: break
        data += d
    s.close()
    return json.loads(data.partition(b"\r\n\r\n")[2])

def count(term):
    return len(req("/api/search?q=" + term + "&limit=50").get("items", []))

p = start()
try:
    unit_dir = os.path.join(IDX, "units", "u%d" % uid)
    idxf = os.path.join(unit_dir, "vol1.idx")
    meta = os.path.join(unit_dir, "unit.json")
    for _ in range(200):
        if os.path.exists(meta): break
        time.sleep(0.05)
    if count("old") != 1:
        print("FAIL baseline old"); sys.exit(1)
    # 用新根重建同路径单元索引，模拟一次原子 rename + epoch 更新
    subprocess.run([BIN, "build", "--root", NEW, "--out", idxf],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.utime(meta, (time.time() + 10, time.time() + 10))
    if count("new") != 1:
        print("FAIL epoch did not load new unit"); sys.exit(1)
    if count("old") != 0:
        print("FAIL old unit still served"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

print("epoch: OK")
