"""Draws ModelHub's icons (no dependencies): python scripts/make_icons.py

  src/manager/res/modelhub.ico        the application (ModelHubManager.exe, ModelHub.exe)
  src/manager/res/tray_running.ico    tray: the service is running (green dot)
  src/manager/res/tray_stopped.ico    tray: the service is stopped (grey dot)
  src/manager/res/tray_busy.ico       tray: starting, stopping or downloading (amber dot)
"""
import os
import struct

OUT = os.path.join(os.path.dirname(__file__), '..', 'src', 'manager', 'res')
SS = 4  # supersampling for smooth edges

BLUE = (0x2F, 0x6B, 0xD8)
WHITE = (0xFF, 0xFF, 0xFF)
DOTS = {'running': (0x22, 0xA8, 0x4A), 'stopped': (0x8A, 0x8F, 0x98), 'busy': (0xE8, 0xA3, 0x17)}


def inside_round_rect(x, y, x0, y0, x1, y1, r):
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def draw(size, dot=None):
    """Returns rows of (r, g, b, a), top row first."""
    n = size * SS
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    x = (px * SS + sx + 0.5) / n  # 0..1
                    y = (py * SS + sy + 0.5) / n
                    c = None
                    if inside_round_rect(x, y, 0.04, 0.04, 0.96, 0.96, 0.2):
                        c = BLUE
                        # three stacked layers: a model's weights
                        for i, top in enumerate((0.24, 0.43, 0.62)):
                            inset = 0.20 + 0.04 * i
                            if inside_round_rect(x, y, inset, top, 1 - inset, top + 0.13, 0.05):
                                c = WHITE
                    if dot:
                        d2 = (x - 0.76) ** 2 + (y - 0.76) ** 2
                        if d2 <= 0.25 ** 2:
                            c = WHITE
                        if d2 <= 0.19 ** 2:
                            c = DOTS[dot]
                    if c:
                        acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; acc[3] += 255
            k = SS * SS
            a = acc[3] / k
            if a == 0:
                row.append((0, 0, 0, 0))
            else:
                row.append(tuple(int(round(v / (acc[3] / 255))) for v in acc[:3]) + (int(round(a)),))
        rows.append(row)
    return rows


def ico(sizes, dot=None):
    images = []
    for s in sizes:
        rows = draw(s, dot)
        pixels = b''.join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)  # bottom-up BGRA
        mask_row = ((s + 31) // 32) * 4
        mask = bytes(mask_row * s)  # alpha decides; AND mask all zero
        header = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0)
        images.append((s, header + pixels + mask))
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for s, data in images:
        out += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b''.join(d for _, d in images)


def main():
    os.makedirs(OUT, exist_ok=True)
    files = {'modelhub.ico': ico([16, 20, 24, 32, 40, 48, 64, 256])}
    for state in DOTS:
        files['tray_%s.ico' % state] = ico([16, 20, 24, 32, 40, 48], state)
    for name, data in files.items():
        with open(os.path.join(OUT, name), 'wb') as f:
            f.write(data)
        print(name, len(data), 'bytes')


if __name__ == '__main__':
    main()
