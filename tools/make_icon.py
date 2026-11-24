#!/usr/bin/env python3
# The icon of SMRPG Recomp (docs/icon/icon.ico and icon_512.png), without dependencies: a rounded square with an indigo
# gradient, a halo, a golden 5-pointed star with an outline and two sparkles. 4x4 supersampling (2x2 in the large
# preview). The .ico has PNG entries (Windows Vista and newer).
# Usage: python tools/make_icon.py  (from the repository's folder)
import math, os, struct, zlib

def star(cx, cy, R, r, n=5, rot=-math.pi / 2):
    pts = []
    for i in range(2 * n):
        a = rot + i * math.pi / n
        rad = R if i % 2 == 0 else r
        pts.append((cx + rad * math.cos(a), cy + rad * math.sin(a)))
    return pts

def inside(poly, x, y):
    c = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c

def lerp(a, b, t):
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(len(a)))

def hexc(h):
    return tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5))

BG_TOP, BG_BOT = hexc('#1b1450'), hexc('#4a2fa0')
GLOW = hexc('#7f6cff')
GOLD_TOP, GOLD_BOT = hexc('#fff08a'), hexc('#f7a21b')
OUTLINE = hexc('#b85a00')
SHINE = hexc('#fffbe0')
WHITE = (1.0, 1.0, 1.0)

def shade(u, v, shapes):
    # u, v in [0, 1]: the point's color (r, g, b, a)
    # background: a rounded square (radius 0.2)
    rr = 0.2
    dx = max(abs(u - 0.5) - (0.5 - rr), 0)
    dy = max(abs(v - 0.5) - (0.5 - rr), 0)
    if dx * dx + dy * dy > rr * rr:
        return (0, 0, 0, 0)
    col = lerp(BG_TOP, BG_BOT, v)
    d = math.hypot(u - 0.5, v - 0.53)  # the halo behind the star
    g = max(0.0, 1 - d / 0.42) ** 2 * 0.55
    col = lerp(col, GLOW, g)
    outline, body, sparkles = shapes
    if inside(outline, u, v):
        col = OUTLINE
    if inside(body, u, v):
        col = lerp(GOLD_TOP, GOLD_BOT, min(1.0, max(0.0, (v - 0.22) / 0.62)))
        # shine: a light band at the top left
        if (u - 0.40) * 0.6 + (v - 0.34) < 0.0 and v < 0.52:
            col = lerp(col, SHINE, 0.45)
    for sp in sparkles:
        if inside(sp, u, v):
            col = WHITE
    return col + (1.0,)

def render(S, k):
    body = star(0.5, 0.545, 0.36, 0.155)
    outline = star(0.5, 0.545, 0.405, 0.185)
    sparkles = [star(0.2, 0.22, 0.085, 0.022, 4, 0), star(0.83, 0.8, 0.06, 0.016, 4, 0)]
    shapes = (outline, body, sparkles)
    rows = []
    for y in range(S):
        row = bytearray()
        for x in range(S):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(k):
                for sx in range(k):
                    c = shade((x + (sx + 0.5) / k) / S, (y + (sy + 0.5) / k) / S, shapes)
                    acc[0] += c[0] * c[3]; acc[1] += c[1] * c[3]; acc[2] += c[2] * c[3]; acc[3] += c[3]
            n = k * k
            a = acc[3] / n
            if a > 0:
                row += bytes((round(acc[0] / acc[3] * 255), round(acc[1] / acc[3] * 255), round(acc[2] / acc[3] * 255), round(a * 255)))
            else:
                row += b'\0\0\0\0'
        rows.append(bytes(row))
    return rows

def png(S, rows):
    raw = b''.join(b'\0' + r for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', S, S, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')

def main():
    out = os.path.join('docs', 'icon')
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [(S, png(S, render(S, 4))) for S in sizes]
    ico = struct.pack('<HHH', 0, 1, len(images))
    off = 6 + 16 * len(images)
    data = b''
    for S, p in images:
        ico += struct.pack('<BBBBHHII', S % 256, S % 256, 0, 0, 1, 32, len(p), off + len(data))
        data += p
    open(os.path.join(out, 'icon.ico'), 'wb').write(ico + data)
    p512 = png(512, render(512, 2))
    open(os.path.join(out, 'icon_512.png'), 'wb').write(p512)
    open(os.path.join(out, 'icon.svg'), 'w', encoding='utf-8').write(svg())
    # (the ones of the macOS and Linux packages inherited from LAS Recomp: icon_1024.png and icon.icns, with PNG inside)
    p1024 = png(1024, render(1024, 2))
    open(os.path.join(out, 'icon_1024.png'), 'wb').write(p1024)
    ents = [(b'ic09', p512), (b'ic10', p1024)]
    body = b''.join(t + struct.pack('>I', 8 + len(d)) + d for t, d in ents)
    open(os.path.join(out, 'icon.icns'), 'wb').write(b'icns' + struct.pack('>I', 8 + len(body)) + body)
    print('docs/icon/icon.ico (%d sizes), icon_512.png, icon_1024.png, icon.icns, icon.svg' % len(sizes))

def svg():  # (the same shape as vectors, 1024 x 1024)
    def pts(p):
        return ' '.join('%.1f,%.1f' % (x * 1024, y * 1024) for x, y in p)
    return '''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1b1450"/><stop offset="1" stop-color="#4a2fa0"/></linearGradient>
    <radialGradient id="glow" cx="0.5" cy="0.53" r="0.42"><stop offset="0" stop-color="#7f6cff" stop-opacity="0.55"/><stop offset="1" stop-color="#7f6cff" stop-opacity="0"/></radialGradient>
    <linearGradient id="gold" x1="0" y1="0.22" x2="0" y2="0.84" gradientUnits="objectBoundingBox"><stop offset="0" stop-color="#fff08a"/><stop offset="1" stop-color="#f7a21b"/></linearGradient>
  </defs>
  <rect width="1024" height="1024" rx="205" fill="url(#bg)"/>
  <rect width="1024" height="1024" rx="205" fill="url(#glow)"/>
  <polygon points="%s" fill="#b85a00"/>
  <polygon points="%s" fill="url(#gold)"/>
  <polygon points="%s" fill="#ffffff"/>
  <polygon points="%s" fill="#ffffff"/>
</svg>
''' % (pts(star(0.5, 0.545, 0.405, 0.185)), pts(star(0.5, 0.545, 0.36, 0.155)), pts(star(0.2, 0.22, 0.085, 0.022, 4, 0)),
       pts(star(0.83, 0.8, 0.06, 0.016, 4, 0)))

if __name__ == '__main__':
    main()
