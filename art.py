#!/usr/bin/env python3
"""Draws the Relay icon (48x48), HOME Menu banner (256x128) and a silent banner sound.

  python3 art.py        writes app/resources/icon.png, banner.png, silence.wav

The mark: one point with two arcs opening right, the signal leaving the console.
Matte near-black field, white mark, same colours as the launcher screens.
"""
import math
import struct
import zlib
from pathlib import Path

BG = (12, 12, 14)
INK = (236, 236, 238)
SS = 4  # supersampling per axis

OUT = Path(__file__).resolve().parent / "app" / "resources"


def coverage(w, h, inside):
    """Fraction of each pixel covered by the shape, sampled SS x SS."""
    cov = []
    for y in range(h):
        row = []
        for x in range(w):
            hit = 0
            for sy in range(SS):
                for sx in range(SS):
                    if inside(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS):
                        hit += 1
            row.append(hit / (SS * SS))
        cov.append(row)
    return cov


def mark(cx, cy, unit):
    """Point at (cx, cy), arcs of radius 2u and 3.4u, stroke 0.7u, 100 degrees wide."""
    dot = unit * 0.85
    arcs = [(unit * 2.0, unit * 0.7), (unit * 3.4, unit * 0.7)]
    half = math.radians(50)

    def inside(x, y):
        dx, dy = x - cx, y - cy
        r = math.hypot(dx, dy)
        if r <= dot:
            return True
        if dx <= 0 or abs(math.atan2(dy, dx)) > half:
            return False
        return any(abs(r - ar) <= sw / 2 for ar, sw in arcs)

    return inside


def png(path, w, h, cov):
    raw = b""
    for row in cov:
        raw += b"\x00" + bytes(
            round(BG[c] + (INK[c] - BG[c]) * a) for a in row for c in range(3))
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


def silence(path, seconds=0.5, rate=22050):
    n = int(seconds * rate)
    data = b"\x00\x00" * n
    path.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
                     + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
                     + b"data" + struct.pack("<I", len(data)) + data)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    # Icon: mark optically centred, the arcs carry more weight than the point.
    png(OUT / "icon.png", 48, 48, coverage(48, 48, mark(15, 24, 5.6)))
    # Banner: shown as a flat card on the HOME Menu top screen, mark at its centre.
    png(OUT / "banner.png", 256, 128, coverage(256, 128, mark(112, 64, 13)))
    silence(OUT / "silence.wav")


if __name__ == "__main__":
    main()
