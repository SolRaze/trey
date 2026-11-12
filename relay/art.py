#!/usr/bin/env python3
"""Renders the Relay HOME Menu icon (48x48, from a 192 render) and banner (256x128), and writes a silent banner sound.

  python3 art.py        writes app/resources/icon.png, banner.png, silence.wav

Icon and banner come from the Steam art page, Code/atlas/steam/art/art.html (kinds ctricon,
ctrbanner, app relay, theme mono), so the CIA and the Steam shortcut share one mark. It renders in
steam-headless on knave, the only chromium with the page's font; knave must be up.
"""
import struct
import subprocess
from pathlib import Path

OUT = Path(__file__).resolve().parent / "app" / "resources"
ART = "/srv/steam/mac/art/art.html"   # live copy inside steam-headless, host /mnt/bak/Games/steam/mac/art


def render(kind, w, h, name):
    shot = f"/tmp/relay-{name}"
    cmd = (f"HOME=/tmp chromium --headless=new --disable-gpu --hide-scrollbars --force-device-scale-factor=1 "
           f"--default-background-color=000000ff --virtual-time-budget=3000 --window-size={w},{h} "
           f"--screenshot={shot} 'file://{ART}#{kind},{w},{h},mono,relay' 2>/dev/null; cat {shot}")
    data = subprocess.run(["ssh", "knave", "docker", "exec", "steam-headless", "sh", "-c", f'"{cmd}"'],
                          capture_output=True, check=True).stdout
    if not data.startswith(b"\x89PNG"):
        raise SystemExit(f"{kind}: no PNG back from knave")
    (OUT / name).write_bytes(data)


def silence(path, seconds=0.5, rate=22050):
    n = int(seconds * rate)
    data = b"\x00\x00" * n
    path.write_bytes(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
                     + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
                     + b"data" + struct.pack("<I", len(data)) + data)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    render("ctricon", 192, 192, "icon-192.png")
    # shrunk smooth: the handheld's outlines and faces drawn at 48 directly turn to mush
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", OUT / "icon-192.png",
                    "-vf", "scale=48:48:flags=lanczos", OUT / "icon.png"], check=True)
    (OUT / "icon-192.png").unlink()
    render("ctrbanner", 256, 128, "banner.png")
    silence(OUT / "silence.wav")


if __name__ == "__main__":
    main()
