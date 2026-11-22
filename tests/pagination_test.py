#!/usr/bin/env python3
"""分页一致性：total 与各页 items 不丢条目、不重复。"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="xl-page-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX); os.makedirs(VOL)
for i in range(1200):
    open(os.path.join(VOL, "pageitem_%04d.txt" % i), "w").write("x")
subprocess.run([BIN, "build", "--root", VOL, "--out", os.path.join(IDX, "vol1.idx")],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def start():
    p = subprocess.Popen([BIN, "serve", "--no-tcp", "--socket", SOCK, "--ui", "ui",
                          "--index-dir", IDX, "--roots", VOL], cwd=ROOT,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(SOCK):
            time.sleep(0.2)
            return p
        if p.poll() is not None:
            raise RuntimeError("serve exited")
        time.sleep(0.05)
    p.kill(); raise RuntimeError("socket timeout")

def req(off):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall(("GET /api/search?q=pageitem&limit=500&offset=%d HTTP/1.1\r\n"
               "Host: x\r\nConnection: close\r\nx-nas-uid: 1000\r\n\r\n" % off).encode())
    out = b""
    while True:
        d = s.recv(65536)
        if not d: break
        out += d
    s.close()
    return json.loads(out.partition(b"\r\n\r\n")[2])

p = start()
try:
    pages = []
    total = None
    for off in (0, 500, 1000):
        time.sleep(0.06)
        d = req(off)
        if total is None: total = d.get("total")
        if d.get("total") != 1200:
            print("FAIL total", d.get("total")); sys.exit(1)
        pages.append(d.get("items", []))
    paths = [x["path"] for page in pages for x in page]
    if len(paths) != 1200:
        print("FAIL page count", len(paths)); sys.exit(1)
    if len(set(paths)) != 1200:
        print("FAIL duplicate paths", len(set(paths))); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
print("pagination: OK")
