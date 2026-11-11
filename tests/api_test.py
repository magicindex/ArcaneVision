#!/usr/bin/env python3
"""HTTP API 基础回归：admin 头优先级、兜底开关、JSON 校验、缩略图缓存。"""
import base64, json, os, socket, subprocess, sys, tempfile, time, urllib.parse

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = tempfile.mkdtemp(prefix="xl-api-")
SOCK = os.path.join(TMP, "app.sock")
IDX = os.path.join(TMP, "idx")
ROOTS = os.path.join(TMP, "root")
os.makedirs(IDX); os.makedirs(ROOTS)
IMG = os.path.join(ROOTS, "pic.png")
# 1x1 透明 PNG；ffmpeg 可解码即可
open(IMG, "wb").write(base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="))

def start(extra_env=None):
    env = dict(os.environ)
    if extra_env: env.update(extra_env)
    p = subprocess.Popen([BIN, "serve", "--no-tcp", "--socket", SOCK, "--ui", "ui",
                          "--index-dir", IDX, "--roots", ROOTS],
                         cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        if os.path.exists(SOCK): return p
        if p.poll() is not None: raise RuntimeError("serve exited")
        time.sleep(0.05)
    p.kill(); raise RuntimeError("socket timeout")

def stop(p):
    p.terminate()
    try: p.wait(timeout=3)
    except Exception: p.kill()
    try: os.unlink(SOCK)
    except FileNotFoundError: pass

def req(method, path, headers=None, body=None):
    s = socket.socket(socket.AF_UNIX); s.connect(SOCK)
    h = f"{method} {path} HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
    for k, v in (headers or {}).items(): h += f"{k}: {v}\r\n"
    if body is not None:
        h += f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n"
    h += "\r\n"
    s.sendall(h.encode() + (body or b""))
    data = b""
    while True:
        d = s.recv(65536)
        if not d: break
        data += d
    s.close()
    head, _, payload = data.partition(b"\r\n\r\n")
    first = head.split(b"\r\n", 1)[0].decode()
    status = int(first.split()[1])
    return status, payload, head.decode(errors="replace")

def expect(name, got, want):
    if got != want:
        print(f"FAIL {name}: got {got!r}, want {want!r}"); sys.exit(1)

p = start()
try:
    base = {"x-nas-uid": "1000"}
    expect("admin fallback", req("GET", "/api/rebuild", base)[0], 405)
    expect("admin header 0", req("GET", "/api/rebuild", dict(base, **{"x-trim-isadmin": "0"}))[0], 403)
    expect("admin header 1", req("GET", "/api/rebuild", dict(base, **{"x-trim-isadmin": "1"}))[0], 405)
    st, body, _ = req("POST", "/api/settings", dict(base, **{"x-trim-isadmin": "1"}), b"{bad json")
    expect("bad json status", st, 200)
    expect("bad json body", json.loads(body).get("error"), "bad json")
    st, body, _ = req("POST", "/api/settings", dict(base, **{"x-trim-isadmin": "1"}), b'{"strictHome":true}')
    expect("settings ok", json.loads(body).get("ok"), True)
    path = urllib.parse.quote(IMG)
    st, body, head = req("GET", "/api/thumb?path=" + path + "&w=96", {"x-nas-uid": "0"})
    if st == 200:
        expect("thumb type", "Content-Type: image/jpeg" in head, True)
        expect("thumb cache", "Cache-Control: private, max-age=86400" in head, True)
        if not body.startswith(b"\xff\xd8"):
            print("FAIL thumb not jpeg"); sys.exit(1)
    else:
        print("SKIP thumbnail (ffmpeg unavailable)")
finally:
    stop(p)

p = start({"XL_ADMIN_FALLBACK": "0"})
try:
    expect("fallback off", req("GET", "/api/rebuild", {"x-nas-uid": "1000"})[0], 403)
    expect("fallback off header", req("GET", "/api/rebuild", {"x-nas-uid": "1000", "x-trim-isadmin": "1"})[0], 405)
finally:
    stop(p)

print("api: OK")
