#!/usr/bin/env python3
"""P6：单元 delta 持久化后，重启/缓存刷新可合并 add/delete。"""
import json
import os
import pwd
import socket
import struct
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="xl-udelta-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol1")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX); os.makedirs(VOL)
uid = os.getuid()
if uid == 0:
    uid = sorted(u.pw_uid for u in pwd.getpwall() if u.pw_uid >= 1000)[0]
open(os.path.join(VOL, "old.txt"), "w").write("old")
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

def rec(path, deleted, size=0, mtime=0):
    op = 1 if deleted else 0
    return struct.pack("<IIBBHII", 0x31444c58, len(path), op, 0, 0, size, mtime) + path.encode()

p = start()
try:
    unit = os.path.join(IDX, "units", "u%d" % uid)
    for _ in range(200):
        if os.path.exists(os.path.join(unit, "unit.json")): break
        time.sleep(0.05)
    if count("old") != 1:
        print("FAIL baseline"); sys.exit(1)
    # 新增文件（不在快照中）+ 删除快照中的 old，写单元 delta
    newp = os.path.join(VOL, "newdelta.txt")
    open(newp, "w").write("new")
    with open(os.path.join(unit, "delta.log"), "ab") as f:
        f.write(rec(newp, False, len("new"), int(time.time())))
        f.write(rec(os.path.join(VOL, "old.txt"), True))
    time.sleep(0.02)
    if count("newdelta") != 1:
        print("FAIL delta add"); sys.exit(1)
    if count("old") != 0:
        print("FAIL delta delete"); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

print("unitdelta: OK")
