"""FLAC formats on trey's /music, one track per album folder, from each file's 42-byte STREAMINFO header.

Listing by `lftp find`, header by `curl -r 0-41`. Old 3DS plays 16-bit up to 48 kHz; anything above is printed.
Run: python3 -I flac-scan.py
"""
import collections
import subprocess
import urllib.parse

HOST = "ftp://5.0.0.8:5000"

listing = subprocess.run(
    ["lftp", "-e", "set net:timeout 20; find /music; quit", HOST],
    capture_output=True, text=True, check=True,
).stdout.splitlines()

seen, sample = set(), []
for path in listing:
    if path.lower().endswith(".flac"):
        album = path.rsplit("/", 1)[0]
        if album not in seen:
            seen.add(album)
            sample.append(path)

counts, heavy = collections.Counter(), []
for path in sample:
    head = subprocess.run(
        ["curl", "-s", "--max-time", "15", "-r", "0-41", HOST + urllib.parse.quote(path)],
        capture_output=True,
    ).stdout[:42]
    if head[:4] != b"fLaC":
        fmt = "no header"
    else:
        si = head[8:]
        fmt = (int.from_bytes(si[10:13], "big") >> 4, ((int.from_bytes(si[12:14], "big") >> 4) & 0x1F) + 1)
    counts[fmt] += 1
    if fmt == "no header" or fmt[0] > 48000 or fmt[1] > 16:
        heavy.append((fmt, path.rsplit("/", 1)[0]))

for fmt, n in counts.most_common():
    print(n, fmt)
for fmt, album in heavy:
    print(fmt, album)
