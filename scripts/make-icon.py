#!/usr/bin/env python3
"""生成玄览图标：圆角渐变底 + 放大镜/索引线。纯标准库 PNG 编码。"""
import struct, sys, zlib

TEAL = (94, 234, 212)
INDIGO = (129, 140, 248)
INK = (8, 14, 26)

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

def in_round_rect(x, y, s, r):
    if x < 0 or y < 0 or x > s or y > s:
        return False
    cx = min(max(x, r), s - r)
    cy = min(max(y, r), s - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r

def glyph(x, y, s):
    """返回 (rgba) 覆盖或 None。坐标归一到 0..1。"""
    u, v = x / s, y / s
    # 镜片
    cx, cy, rr = 0.455, 0.455, 0.215
    d = ((u - cx) ** 2 + (v - cy) ** 2) ** 0.5
    ring = 0.062
    if rr - ring <= d <= rr:
        return (255, 255, 255, 255)
    # 手柄
    hx0, hy0, hx1, hy1 = 0.605, 0.605, 0.775, 0.775
    t = ((u - hx0) * (hx1 - hx0) + (v - hy0) * (hy1 - hy0)) / ((hx1 - hx0) ** 2 + (hy1 - hy0) ** 2)
    t = min(max(t, 0.0), 1.0)
    px, py = hx0 + (hx1 - hx0) * t, hy0 + (hy1 - hy0) * t
    hd = ((u - px) ** 2 + (v - py) ** 2) ** 0.5
    if hd <= 0.035:
        return (255, 255, 255, 255)
    # 镜片内的三条索引线
    if d <= rr - ring - 0.018:
        for ly in (-0.075, 0.0, 0.075):
            if abs((v - cy) - ly) <= 0.019 and abs(u - cx) <= 0.105:
                return (255, 255, 255, 235)
    return None

def render(size, ss=4):
    n = size * ss
    r = n * 0.225
    rows = []
    for py in range(size):
        row = bytearray()
        for px in range(size):
            acc = [0, 0, 0, 0]
            cnt = ss * ss
            for sy in range(ss):
                for sx in range(ss):
                    x = px * ss + sx + 0.5
                    y = py * ss + sy + 0.5
                    if not in_round_rect(x, y, n, r):
                        continue
                    g = glyph(x, y, n)
                    if g:
                        c = g
                    else:
                        t = (x / n * 0.55 + y / n * 0.45)
                        c = lerp(TEAL, INDIGO, t) + (255,)
                    # 预乘平均（简单处理，图标足够）
                    acc[0] += c[0] * c[3]
                    acc[1] += c[1] * c[3]
                    acc[2] += c[2] * c[3]
                    acc[3] += c[3]
            a = acc[3] / cnt
            if a <= 0:
                row += b'\x00\x00\x00\x00'
            else:
                row += bytes((int(acc[0] / acc[3]), int(acc[1] / acc[3]), int(acc[2] / acc[3]), int(a)))
        rows.append(bytes(row))
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    for s in (64, 256):
        data = render(s)
        for path in (f'{out}/ICON{"_256" if s == 256 else ""}.PNG',
                     f'{out}/icon_{s}.png'):
            with open(path, 'wb') as f:
                f.write(data)
        print('icon', s, 'ok')
