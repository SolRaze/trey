#!/usr/bin/env python3
"""Relay's knave side: save sync with Azahar, the CIA inbox and the Azahar wake call, plain HTTP.

  python3 knave.py [port]      default 8740, run by relay-knave.service

LAN only, no auth: anything that reaches the port can read and replace Azahar saves.

Save pack, the wire form both ways (app/sources/saves.cpp writes and reads the same):
  per file: u16 path length, path (utf-8, '/' separated, no leading '/'), u32 size, data; little endian
  ends with a u16 0; files sorted by path, so one save always packs to the same bytes
  hash: FNV-1a 64 over the whole pack, as 16 lower-case hex digits

  GET  /saves/<tid>          pack of Azahar's save, 404 when it has none
  GET  /saves/<tid>/hash     its hash, 404 when none
  PUT  /saves/<tid>          replaces Azahar's save; the old one goes to backups first
  PUT  /saves/<tid>/backup   keeps a pack as a backup only (the console's copy before a pull)
  GET  /inbox                one line per CIA waiting: name, tab, size
  GET  /inbox/<name>         the CIA
  POST /inbox/<name>/done    moves it to inbox/done
  POST /azahar               starts the Steam "Relay" target in steam-headless unless it runs
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

ROOT = Path("/srv/relay")
INBOX = ROOT / "inbox"
BACKUPS = ROOT / "saves"
# Azahar's emulated SD inside steam-headless (/home/sol there); id0 and id1 are all zeros in Azahar.
# The unit grants CAP_DAC_READ_SEARCH: /srv/docker is root 0710.
ZERO = "0" * 32
AZAHAR_TITLES = Path("/srv/docker/volumes/steamhost_steamhome/_data/.local/share/azahar-emu/sdmc"
                     f"/Nintendo 3DS/{ZERO}/{ZERO}/title")
RUN = ["docker", "exec", "-d", "-u", "sol", "-e", "DISPLAY=:0", "-e", "XAUTHORITY=/xauth/Xauthority",
       "-e", "HOME=/home/sol", "-e", "XDG_RUNTIME_DIR=/home/sol/run", "steam-headless", "/srv/steam/azahar/run.sh"]
TID = re.compile(r"^[0-9a-f]{16}$")
CIA = re.compile(r"^[^/]+\.cia$", re.I)


def save_dir(tid):
    return AZAHAR_TITLES / tid[:8] / tid[8:] / "data" / "00000001"


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def pack(folder):
    # sorted on the path bytes, the order the console sorts in
    files = sorted((p.relative_to(folder).as_posix().encode(), p) for p in folder.rglob("*") if p.is_file())
    out = bytearray()
    for rel, p in files:
        data = p.read_bytes()
        out += struct.pack("<H", len(rel)) + rel + struct.pack("<I", len(data)) + data
    return bytes(out + struct.pack("<H", 0))


def unpack(blob, folder):
    """Writes a pack into folder. Raises ValueError on a malformed pack before touching anything."""
    files, i = [], 0
    while True:
        (n,) = struct.unpack_from("<H", blob, i); i += 2
        if n == 0:
            break
        rel = blob[i:i + n].decode(); i += n
        (size,) = struct.unpack_from("<I", blob, i); i += 4
        if i + size > len(blob) or rel.startswith("/") or ".." in rel.split("/"):
            raise ValueError("bad pack")
        files.append((rel, blob[i:i + size])); i += size
    if i != len(blob):
        raise ValueError("trailing bytes")
    if folder.exists():
        shutil.rmtree(folder)
    for rel, data in files:
        dst = folder / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)


def backup(tid, blob, kind):
    d = BACKUPS / tid
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{time.strftime('%Y%m%d-%H%M%S')}-{kind}.pack").write_bytes(blob)


def azahar_running():
    r = subprocess.run(["docker", "exec", "steam-headless", "pgrep", "-f", "azahar"], capture_output=True)
    return r.returncode == 0


class H(BaseHTTPRequestHandler):
    def send(self, code, body=b"", ctype="text/plain"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def parts(self):
        return [unquote(p) for p in self.path.split("?")[0].split("/") if p]

    def do_GET(self):
        p = self.parts()
        if len(p) >= 2 and p[0] == "saves" and TID.match(p[1]):
            d = save_dir(p[1])
            if not d.is_dir() or not any(d.rglob("*")):
                return self.send(404, "no save")
            blob = pack(d)
            if p[2:] == ["hash"]:
                return self.send(200, fnv(blob))
            if not p[2:]:
                return self.send(200, blob, "application/octet-stream")
        if p == ["inbox"]:
            INBOX.mkdir(parents=True, exist_ok=True)
            rows = [f"{f.name}\t{f.stat().st_size}" for f in sorted(INBOX.iterdir()) if f.is_file() and CIA.match(f.name)]
            return self.send(200, "".join(r + "\n" for r in rows))
        if len(p) == 2 and p[0] == "inbox" and CIA.match(p[1]) and (INBOX / p[1]).is_file():
            return self.send(200, (INBOX / p[1]).read_bytes(), "application/octet-stream")
        self.send(404, "not found")

    def do_PUT(self):
        p = self.parts()
        if len(p) >= 2 and p[0] == "saves" and TID.match(p[1]):
            blob = self.body()
            if p[2:] == ["backup"]:
                backup(p[1], blob, "console")
                return self.send(200, fnv(blob))
            if not p[2:]:
                d = save_dir(p[1])
                if d.is_dir() and any(d.rglob("*")):
                    backup(p[1], pack(d), "azahar")
                try:
                    unpack(blob, d)
                except (ValueError, struct.error, UnicodeDecodeError) as e:
                    return self.send(400, str(e))
                return self.send(200, fnv(blob))
        self.send(404, "not found")

    def do_POST(self):
        p = self.parts()
        if len(p) == 3 and p[0] == "inbox" and p[2] == "done" and CIA.match(p[1]) and (INBOX / p[1]).is_file():
            (INBOX / "done").mkdir(exist_ok=True)
            os.replace(INBOX / p[1], INBOX / "done" / p[1])
            return self.send(200, "moved")
        if p == ["azahar"]:
            if azahar_running():
                return self.send(200, "already running")
            r = subprocess.run(RUN, capture_output=True)
            return self.send(200 if r.returncode == 0 else 500, "started" if r.returncode == 0 else r.stderr)
        self.send(404, "not found")

    def log_message(self, fmt, *args):
        sys.stderr.write(f"{self.address_string()} {fmt % args}\n")


if __name__ == "__main__":
    ThreadingHTTPServer(("", int(sys.argv[1]) if len(sys.argv) > 1 else 8740), H).serve_forever()
