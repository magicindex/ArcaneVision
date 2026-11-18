#!/usr/bin/env python3
"""P3：共享扫描后按单元生成 idx/units/u<uid>，并可被 QueryRouter 命中。"""
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
TMP = tempfile.mkdtemp(prefix="xl-unit-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol1")
SOCK = os.path.join(TMP, "app.sock")
os.makedirs(IDX); os.makedirs(VOL)
uid = os.getuid()
if uid == 0:
    uid = sorted(u.pw_uid for u in pwd.getpwall() if u.pw_uid >= 1000)[0]
open(os.path.join(VOL, "unitonly.txt"), "w").write("unit")
os.makedirs(os.path.join(VOL, "@team", "teamA"))
open(os.path.join(VOL, "@team", "teamA", "teamonly.txt"), "w").write("team")

gid = os.getgid()
open(os.path.join(TMP, "scope.json"), "w").write(json.dumps({
    "version": 2, "scanMode": "hybrid",
    "shareGroups": [], "isolatedGroups": [gid],
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

def req(path, extra=""):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    s.sendall(("GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
               "x-nas-uid: %d\r\n%s\r\n" % (uid, extra)).encode())
    data = b""
    while True:
        d = s.recv(65536)
        if not d: break
        data += d
    s.close()
    return json.loads(data.partition(b"\r\n\r\n")[2])

p = start()
try:
    unit_dir = os.path.join(IDX, "units", "u%d" % uid)
    unit_idx = os.path.join(unit_dir, "vol1.idx")
    unit_json = os.path.join(unit_dir, "unit.json")
    for _ in range(200):
        if os.path.exists(unit_json) and os.path.exists(unit_idx): break
        time.sleep(0.05)
    if not os.path.exists(unit_idx) or not os.path.exists(unit_json):
        print("FAIL unit index not generated"); sys.exit(1)
    meta = json.load(open(unit_json))
    if meta.get("unit") != "u%d" % uid or meta.get("kind") != "user":
        print("FAIL unit.json", meta); sys.exit(1)
    gdir = os.path.join(IDX, "units", "g%d" % gid)
    for _ in range(200):
        if os.path.exists(os.path.join(gdir, "unit.json")): break
        time.sleep(0.05)
    if not os.path.exists(os.path.join(gdir, "vol1.idx")):
        print("FAIL group unit not generated"); sys.exit(1)
    gmeta = json.load(open(os.path.join(gdir, "unit.json")))
    if gmeta.get("kind") != "group" or gmeta.get("gid") != gid:
        print("FAIL group unit.json", gmeta); sys.exit(1)
    d = req("/api/search?q=unitonly&limit=50")
    if len(d.get("items", [])) != 1:
        print("FAIL unit route after build", d); sys.exit(1)
    d = req("/api/search?q=teamonly&limit=50")
    if len(d.get("items", [])) != 1:
        print("FAIL @team not scanned into unit", d); sys.exit(1)
    c = req("/api/scanmode/candidates", "x-trim-isadmin: 1\r\n")
    if not any(x.get("path", "").endswith("/@team/teamA") for x in c.get("directories", [])):
        print("FAIL candidates missing @team", c); sys.exit(1)
finally:
    p.terminate()
    try: p.wait(timeout=5)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

print("unitbuild: OK")
