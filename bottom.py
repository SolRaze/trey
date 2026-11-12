#!/usr/bin/env python3
"""Puts Azahar's bottom screen on folio's Extend app while a game runs; touches come back as clicks.

Runs on knave's host as a watcher against steam-headless's X server :0. Azahar uses the
Separate Windows layout: the fullscreen main window is the top screen on HDMI, the
"Secondary Window" is the bottom screen, also fullscreen. Its WM frame is redirected
offscreen with XComposite (manual), so it never paints over the top screen, yet it
still takes pointer input and x11grab still reads it by id.

folio's app connects to its own 127.0.0.1:54400, which `adb reverse` maps here. Wire
format is Extend's, defined at the top of Devices/folio/extend/mac/extend.swift:
  knave to folio: u8 type | u32 length (big-endian) | payload
    0 config: u16 width | u16 height | u16 spsLength | sps | pps
    1 frame:  Annex B NAL units of one access unit
  folio to knave, 5 bytes: u8 action (0 down, 1 move, 2 up) | u16 x | u16 y, 0...65535
Mac Extend reverses the same port on folio: while it runs, this one waits.
"""

import ctypes
import itertools
import os
import re
import socket
import struct
import subprocess
import threading
import time

FOLIO = "folio.tail13ed40.ts.net:5555"
APP = "local.sol.extend/.Main"
PORT = 54400
DISPLAY = ":0"
os.environ.setdefault("XAUTHORITY", "/srv/steamhost/xauth/Xauthority")
os.environ["DISPLAY"] = DISPLAY
VIDEO = (1280, 960)  # 4:3 like the bottom screen; folio aspect-fits it
BITRATE = "6M"


def sh(*cmd):
    return subprocess.run(cmd, check=False, capture_output=True, text=True).stdout


def adb(*args):
    return sh("adb", "-s", FOLIO, *args)


def secondary():
    """Window id of Azahar's bottom-screen window while a game runs, else None."""
    ids = sh("xdotool", "search", "--onlyvisible", "--name", r"^Azahar .*\| Secondary Window$").split()
    return int(ids[0]) if ids else None


def geometry(win):
    info = sh("xwininfo", "-id", str(win))
    g = dict(re.findall(r"(Absolute upper-left [XY]|Width|Height):\s+(-?\d+)", info))
    return int(g["Absolute upper-left X"]), int(g["Absolute upper-left Y"]), int(g["Width"]), int(g["Height"])


def screen(w, h):
    """The 320x240 bottom screen inside a w x h window: Azahar letterboxes it centred."""
    s = min(w / 320, h / 240)
    sw, sh_ = int(320 * s) & ~1, int(240 * s) & ~1
    return (w - sw) // 2, (h - sh_) // 2, sw, sh_


class X:
    """XTest pointer and XComposite redirect on :0."""

    def __init__(self):
        x11 = ctypes.CDLL("libX11.so.6")
        self.xt = ctypes.CDLL("libXtst.so.6")
        self.xc = ctypes.CDLL("libXcomposite.so.1")
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XFlush.argtypes = [ctypes.c_void_p]
        self.flush = lambda: x11.XFlush(self.dpy)
        self.dpy = x11.XOpenDisplay(DISPLAY.encode())
        if not self.dpy:
            raise SystemExit(f"cannot open {DISPLAY}")
        self.xt.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xc.XCompositeRedirectWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int]
        self.redirected = None

    def redirect(self, win):
        """Manual redirect of the WM frame around win: it renders offscreen only.

        Redirecting the client window alone still paints it; the frame is the toplevel.
        Held while this connection lives."""
        frame = int(re.search(r"Parent window id: (0x[0-9a-f]+)", sh("xwininfo", "-id", str(win), "-children")).group(1), 16)
        if frame != self.redirected:
            self.xc.XCompositeRedirectWindow(self.dpy, frame, 1)
            self.flush()
            self.redirected = frame

    def touch(self, action, x, y):
        self.xt.XTestFakeMotionEvent(self.dpy, -1, x, y, 0)
        if action in (0, 2):
            self.xt.XTestFakeButtonEvent(self.dpy, 1, action == 0, 0)
        self.flush()


def place(win):
    """Keeps the bottom window, fullscreen like the main one, on top for input.

    Azahar's fullscreen covers both windows; the WM ignores moves and resizes."""
    # Focus moves the fullscreen main window out of openbox's fullscreen layer, so the
    # raised bottom window stays above it and takes the clicks.
    sh("xdotool", "windowactivate", str(win))
    sh("xdotool", "windowraise", str(win))


def nals(au):
    """Splits Annex B bytes into NAL payloads without start codes."""
    return [p for p in re.split(rb"\x00\x00\x00\x01|\x00\x00\x01", au) if p]


def send(conn, kind, payload):
    conn.sendall(struct.pack(">BI", kind, len(payload)) + payload)


def touches(conn, win, x):
    held = None  # last point while the finger is down, released if folio drops mid-touch
    rect = None
    try:
        while True:
            msg = conn.recv(5, socket.MSG_WAITALL)
            if len(msg) < 5:
                return
            action, nx, ny = struct.unpack(">BHH", msg)
            if action == 0 or rect is None:
                wx, wy, ww, wh = geometry(win)
                cx, cy, w, h = screen(ww, wh)
                rect = (wx + cx, wy + cy, w, h)
            x0, y0, w, h = rect
            p = (x0 + nx * (w - 1) // 65535, y0 + ny * (h - 1) // 65535)
            x.touch(action, *p)
            held = None if action == 2 else p
    except OSError:
        pass
    finally:
        if held:
            x.touch(2, *held)


def stream(conn, win, x):
    _, _, ww, wh = geometry(win)
    cx, cy, w, h = screen(ww, wh)
    print(f"streaming window {win:#x}, bottom screen {w}x{h}+{cx}+{cy}", flush=True)
    ff = subprocess.Popen([
        "ffmpeg", "-loglevel", "error", "-f", "x11grab", "-window_id", str(win), "-draw_mouse", "0",
        "-framerate", "60", "-i", DISPLAY,
        "-vf", f"crop={w}:{h}:{cx}:{cy},scale={VIDEO[0]}:{VIDEO[1]}:flags=lanczos,format=yuv420p",
        "-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ull", "-zerolatency", "1", "-bf", "0",
        "-g", "120", "-rc", "cbr", "-b:v", BITRATE, "-profile:v", "high",
        "-bsf:v", "h264_metadata=aud=insert", "-f", "h264", "-",
    ], stdout=subprocess.PIPE)
    threading.Thread(target=touches, args=(conn, win, x), daemon=True).start()
    buf, configured = b"", False
    aud = re.compile(rb"\x00\x00\x00\x01\x09")
    try:
        while chunk := ff.stdout.read1(1 << 16):
            buf += chunk
            starts = [m.start() for m in aud.finditer(buf)]
            # Every complete access unit runs from one AUD to the next.
            for a, b in itertools.pairwise(starts):
                units = nals(buf[a:b])
                if not configured:
                    sps = next((n for n in units if n[0] & 0x1F == 7), None)
                    pps = next((n for n in units if n[0] & 0x1F == 8), None)
                    if not (sps and pps):
                        continue
                    send(conn, 0, struct.pack(">HHH", *VIDEO, len(sps)) + sps + pps)
                    configured = True
                frame = b"".join(b"\x00\x00\x00\x01" + n for n in units if n[0] & 0x1F not in (7, 8, 9))
                if frame:
                    send(conn, 1, frame)
            if len(starts) > 1:
                buf = buf[starts[-1]:]
    except OSError:
        pass
    finally:
        ff.kill()
        conn.close()
        print("stream ended", flush=True)


def main():
    srv = socket.create_server(("127.0.0.1", PORT))
    srv.settimeout(3)
    x = X()
    shown = None  # window folio was switched to
    while True:
        win = secondary()
        if not win:
            if shown:
                adb("shell", "am", "force-stop", APP.split("/")[0])
                print("game gone, folio released", flush=True)
                shown = None
            time.sleep(2)
            continue
        if win != shown:
            x.redirect(win)
            place(win)
            sh("adb", "connect", FOLIO)
            if "error" in adb("reverse", f"tcp:{PORT}", f"tcp:{PORT}").lower():
                time.sleep(5)  # Mac Extend holds the port; retry
                continue
            adb("shell", "am", "start", "-n", APP)
            print(f"game up, folio switched to window {win:#x}", flush=True)
            shown = win
        try:
            conn, _ = srv.accept()
        except TimeoutError:
            continue
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        stream(conn, win, x)


if __name__ == "__main__":
    main()
