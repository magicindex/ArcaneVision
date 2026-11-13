#!/usr/bin/env python3
"""USB 挂载模拟回归：要求事先把 tmpfs/测试目录挂到 /vol00/fake。"""
import json, os, pwd, socket, subprocess, sys, tempfile, time, urllib.parse

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "bin/xuanlan")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOUNT = "/vol00/fake"
if not os.path.isdir(MOUNT):
    if os.environ.get("XL_USB_REQUIRED"):
        print(f"FAIL: {MOUNT} not mounted"); sys.exit(1)
    print(f"SKIP USB test ({MOUNT} not mounted)"); sys.exit(0)

TMP = tempfile.mkdtemp(prefix="xl-usb-")
SOCK = os.path.join(TMP, "app.sock")
IDX = os.path.join(TMP, "idx")
ROOTDIR = os.path.join(TMP, "root")
os.makedirs(IDX); os.makedirs(ROOTDIR)

def start():
    p = subprocess.Popen([BIN, "serve", "--no-tcp", "--socket", SOCK, "--ui", "ui",
                          "--index-dir", IDX, "--roots", ROOTDIR],
                         cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
    status = int(head.split(b"\r\n", 1)[0].split()[1])
    return status, payload

def jget(payload):
    try: return json.loads(payload)
    except Exception: return {}

uids = sorted(u.pw_uid for u in pwd.getpwall() if u.pw_uid >= 1000)
uid = uids[0] if uids else 0
hdr = {"x-nas-uid": str(uid), "x-trim-isadmin": "1"}

p = start()
try:
    st, body = req("POST", "/api/settings", hdr, b'{"usbEnabled":true}')
    if st != 200 or not jget(body).get("ok"):
        print("FAIL enable usb:", st, body[:120]); sys.exit(1)
    ok = False
    for _ in range(80):
        st, body = req("GET", "/api/settings", hdr)
        d = jget(body)
        ms = d.get("usbMounts") or []
        if any(m.get("path") == MOUNT and m.get("ok") for m in ms):
            ok = True; break
        time.sleep(0.25)
    if not ok:
        print("FAIL usb mount not indexed"); sys.exit(1)
    st, body = req("GET", "/api/search?q=marker&limit=10", hdr)
    d = jget(body)
    hits = [i for i in (d.get("items") or []) if i.get("usb") and i.get("path") == MOUNT + "/marker.txt"]
    if not hits:
        print("FAIL usb search:", body[:300]); sys.exit(1)
    print("usb: OK")
finally:
    stop(p)
