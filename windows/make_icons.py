#!/usr/bin/env python3
"""Render the zapret shield icons as Windows .ico files (pure Python, no deps).

Same shield outline as data/icons/*.svg, but colored, since the Windows tray
does not recolor "symbolic" icons:

  zapret-on.ico   solid green shield with a white check   (zapret running)
  zapret-off.ico  grey outlined shield                     (zapret stopped)

Shapes are drawn from signed distance fields, which gives cheap anti-aliasing
for both fills and strokes at every icon size.

usage: make_icons.py OUT_DIR
"""

import math
import os
import struct
import sys
import zlib

SIZES = (16, 20, 24, 32, 40, 48, 64, 256)

GREEN = (0x1E, 0x9E, 0x55)
GREEN_EDGE = (0x14, 0x6E, 0x3B)
GREY = (0x8C, 0x8C, 0x8C)
WHITE = (0xFF, 0xFF, 0xFF)


def _bezier(p0, p1, p2, p3, steps=16):
    pts = []
    for i in range(1, steps + 1):
        t = i / steps
        u = 1 - t
        pts.append((
            u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
            u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1],
        ))
    return pts


def shield_polygon():
    """The SVG path, in its 16x16 viewBox:
    M8 1 L13.5 3.1 V8 C13.5 11.6 11.1 14 8 15.2 C4.9 14 2.5 11.6 2.5 8 V3.1 Z"""
    pts = [(8, 1), (13.5, 3.1), (13.5, 8)]
    pts += _bezier((13.5, 8), (13.5, 11.6), (11.1, 14), (8, 15.2))
    pts += _bezier((8, 15.2), (4.9, 14), (2.5, 11.6), (2.5, 8))
    pts += [(2.5, 3.1)]
    return pts


CHECK = [(5.3, 8.1), (7.2, 10.0), (10.9, 6.0)]


def _seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    ex, ey = ax + t * dx - px, ay + t * dy - py
    return math.sqrt(ex * ex + ey * ey)


def _poly_sdf(px, py, poly):
    """Signed distance to a closed polygon: negative inside."""
    d = float("inf")
    inside = False
    n = len(poly)
    for i in range(n):
        ax, ay = poly[i]
        bx, by = poly[(i + 1) % n]
        d = min(d, _seg_dist(px, py, ax, ay, bx, by))
        if (ay > py) != (by > py) and px < (bx - ax) * (py - ay) / (by - ay) + ax:
            inside = not inside
    return -d if inside else d


def _line_dist(px, py, pts):
    return min(
        _seg_dist(px, py, *pts[i], *pts[i + 1]) for i in range(len(pts) - 1)
    )


def _cov(d):
    """Coverage of a pixel whose center is at signed distance d (in pixels)."""
    return max(0.0, min(1.0, 0.5 - d))


def _over(dst, src, a):
    """Composite straight-alpha color src with opacity a over dst (r,g,b,a)."""
    r, g, b, da = dst
    oa = a + da * (1 - a)
    if oa <= 0:
        return (0, 0, 0, 0.0)
    mix = lambda s, d: (s * a + d * da * (1 - a)) / oa  # noqa: E731
    return (mix(src[0], r), mix(src[1], g), mix(src[2], b), oa)


def render(size, on):
    shield = shield_polygon()
    scale = size / 16.0  # viewBox units -> pixels
    edge = max(1.0, size / 16.0)  # stroke widths in pixels
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            px, py = (x + 0.5) / scale, (y + 0.5) / scale
            d = _poly_sdf(px, py, shield) * scale  # pixels
            c = (0, 0, 0, 0.0)
            if on:
                c = _over(c, GREEN_EDGE, _cov(d))
                c = _over(c, GREEN, _cov(d + 0.9 * edge))
                w = 1.6 * edge if size >= 24 else 1.3 * edge
                dc = _line_dist(px, py, CHECK) * scale - w / 2
                c = _over(c, WHITE, _cov(dc) * _cov(d))
            else:
                w = 1.5 * edge
                c = _over(c, GREY, _cov(abs(d + w / 2) - w / 2))
            row += bytes((
                round(c[0]), round(c[1]), round(c[2]), round(c[3] * 255)
            ))
        rows.append(bytes(row))
    return rows


def png(size, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def ico(images):
    """images: list of (size, png_bytes). PNG-in-ICO works on Vista and later."""
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    blobs = b""
    for size, data in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    return out + blobs


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out_dir, exist_ok=True)
    for name, on in (("zapret-on.ico", True), ("zapret-off.ico", False)):
        images = [(s, png(s, render(s, on))) for s in SIZES]
        with open(os.path.join(out_dir, name), "wb") as fh:
            fh.write(ico(images))
        if os.environ.get("ZAPRET_ICON_PREVIEW"):
            with open(os.path.join(out_dir, name[:-4] + "-preview.png"), "wb") as fh:
                fh.write(images[-1][1])


if __name__ == "__main__":
    main()
