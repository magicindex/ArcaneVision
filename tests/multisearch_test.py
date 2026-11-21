#!/usr/bin/env python3
"""多词搜索：全部/任一、引号精确词组、大小写、顺序、分隔符。"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="xl-multi-")
IDX = os.path.join(TMP, "idx")
VOL = os.path.join(TMP, "vol")
SOCK = os.path.join(TMP, "app.sock")
SETTINGS = os.path.join(TMP, "settings.json")
try:
    os.unlink(SETTINGS)
except FileNotFoundError:
    pass
os.makedirs(IDX)
os.makedirs(VOL)

files = [
    "量子计算指南.txt",
    "量子 计算.txt",
    "计算 量子.txt",
    "量子 研究 计算.txt",
    "Quantum Computing.txt",
    "quantum notes.txt",
    "Alpha Beta.txt",
    "beta alpha.txt",
    "alpha only.txt",
    "tango alpha.txt",
]
for name in files:
    open(os.path.join(VOL, name), "w").write("x")

subprocess.run([BIN, "build", "--root", VOL, "--out", os.path.join(IDX, "vol1.idx")],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def start():
    p = subprocess.Popen([BIN, "serve", "--no-tcp", "--socket", SOCK, "--ui", "ui",
                          "--index-dir", IDX, "--roots", VOL],
                         cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(SOCK):
            # socket 出现后索引/设置可能还在加载，等 status 就绪再查询
            for _ in range(100):
                try:
                    st = req("/api/status")
                    if st.get("state") in ("ready", "degraded"):
                        return p
                except Exception:
                    pass
                time.sleep(0.05)
            return p
        if p.poll() is not None:
            raise RuntimeError("serve exited")
        time.sleep(0.05)
    p.kill()
    raise RuntimeError("socket timeout")

def req(path, extra="", body=None):
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    if body is None:
        data = ("GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                "x-nas-uid: 1000\r\n%s\r\n" % extra).encode()
    else:
        data = ("POST " + path + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                "Content-Type: application/json\r\nContent-Length: %d\r\n"
                "x-nas-uid: 1000\r\n%s\r\n%s" % (len(body), extra, body)).encode()
    s.sendall(data)
    out = b""
    while True:
        d = s.recv(65536)
        if not d:
            break
        out += d
    s.close()
    return json.loads(out.partition(b"\r\n\r\n")[2])

def count(q):
    d = req("/api/search?q=" + __import__("urllib.parse").parse.quote(q) + "&limit=50")
    return len(d.get("items", []))

def setting(body):
    req("/api/settings", "x-trim-isadmin: 1\r\n", json.dumps(body))

def expect(name, got, want):
    if got != want:
        print("FAIL %s: got %r want %r" % (name, got, want))
        sys.exit(1)

p = start()
try:
    # 默认：同时命中、任意顺序、不区分大小写、空格分隔
    expect("all terms any order", count("量子 计算"), 4)
    expect("phrase exact", count('"量子 计算"'), 1)
    expect("phrase no space", count('"量子计算"'), 1)
    expect("case-insensitive", count("quantum"), 2)

    # 单一命中
    setting({"searchMatchAll": False})
    expect("any terms", count("alpha beta"), 4)
    setting({"searchMatchAll": True})
    expect("all terms", count("alpha beta"), 2)

    # 大小写
    setting({"searchCaseSensitive": True})
    expect("case-sensitive", count("quantum"), 1)
    setting({"searchCaseSensitive": False})

    # 顺序
    setting({"searchOrdered": True})
    expect("ordered", count("alpha beta"), 1)
    setting({"searchOrdered": False})
    expect("unordered", count("alpha beta"), 2)

    # 逗号分隔
    setting({"searchSeparator": "comma"})
    expect("comma", count("alpha,beta"), 2)
    expect("chinese comma", count("alpha，beta"), 2)
    setting({"searchSeparator": "line"})
    expect("line", count("alpha\nbeta"), 2)
    setting({"searchSeparator": "space"})
    expect("space does not split comma", count("alpha,beta"), 0)

    # 总开关
    setting({"searchMulti": False})
    expect("multi off", count("alpha beta"), 1)
    setting({"searchMulti": True})

    # 设置回显
    d = req("/api/settings", "x-trim-isadmin: 1\r\n")
    expect("setting echo", (d.get("searchMulti"), d.get("searchMatchAll"), d.get("searchSeparator")),
           (True, True, "space"))
finally:
    p.terminate()
    try:
        p.wait(timeout=5)
    except Exception:
        p.kill()
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    try:
        os.unlink(SETTINGS)
    except FileNotFoundError:
        pass

print("multisearch: OK")
