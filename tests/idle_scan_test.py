#!/usr/bin/env python3
"""自动扫描设置：默认关闭，支持 daily/interval/idle，并持久化。"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="xl-idle-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX)
os.makedirs(VOL)
open(os.path.join(VOL, "idle.txt"), "w").write("x")
subprocess.run([BIN, "build", "--root", VOL, "--out", os.path.join(IDX, "vol1.idx")],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def start():
    p = subprocess.Popen([BIN, "serve", "--no-tcp", "--socket", SOCK, "--ui", "ui",
                          "--index-dir", IDX, "--roots", VOL],
                         cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(SOCK):
            time.sleep(0.2)
            return p
        if p.poll() is not None:
            raise RuntimeError("serve exited")
        time.sleep(0.05)
    p.kill()
    raise RuntimeError("socket timeout")

def req(path, body=None):
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    if body is None:
        h = "GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\nx-nas-uid: 1000\r\n\r\n"
    else:
        b = json.dumps(body)
        h = ("POST " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
             "Content-Type: application/json\r\nContent-Length: %d\r\n"
             "x-nas-uid: 1000\r\nx-trim-isadmin: 1\r\n\r\n%s" % (len(b), b))
    s.sendall(h.encode())
    out = b""
    while True:
        d = s.recv(65536)
        if not d:
            break
        out += d
    s.close()
    return json.loads(out.partition(b"\r\n\r\n")[2])

p = start()
try:
    d = req("/api/settings")
    if d.get("rebuild", {}).get("mode") != "off":
        print("FAIL default rebuild mode", d.get("rebuild")); sys.exit(1)
    for mode in ("daily", "interval", "idle"):
        req("/api/settings", {"rebuild": {"mode": mode, "hour": 3, "minute": 0, "hours": 6}})
        d = req("/api/settings")
        if d.get("rebuild", {}).get("mode") != mode:
            print("FAIL mode", mode, d.get("rebuild")); sys.exit(1)
    req("/api/settings", {"rebuild": {"mode": "off", "hour": 3, "minute": 0, "hours": 6}})
    d = req("/api/settings")
    if d.get("rebuild", {}).get("mode") != "off":
        print("FAIL restore off", d.get("rebuild")); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

print("idle-scan: OK")
