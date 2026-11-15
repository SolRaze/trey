#!/usr/bin/env python3
"""Tunnel broker for Relay: the console dials out to it, FTP clients dial in, it joins the two.

  python3 tunnel.py --secret FILE [--tunnel ADDR:PORT] [--ftp ADDR:PORT] [--tls CERT KEY]

  --secret   file holding the console's shared secret, the same text as sdmc:/3ds/relay/tunnel's
             `secret` line; trailing whitespace ignored
  --tunnel   where the console connects, default 127.0.0.1:8741
  --ftp      where FTP clients connect, default 127.0.0.1:5000; `trey` reaches it by TREY_HOST
  --tls      the broker terminates TLS itself (LAN test with a self-signed pair, console pins it);
             without it the tunnel port is plain TCP and must only be reached through a
             TLS-terminating front (Tailscale Funnel --tls-terminated-tcp), so it binds loopback

Wire, one TLS stream from the console, carrying many TCP streams. Every frame:
  u8 type, u16 stream id, u16 payload length, payload; big endian; payload at most 16384
  CHAL  1  broker -> console, id 0, 32 random bytes, first frame on the stream
  AUTH  2  console -> broker, id 0, HMAC-SHA256(secret, nonce + b"relay-tunnel 1")
           nothing else is accepted before it; a wrong one closes the stream, no answer
  OPEN  3  broker -> console, u16 port: connect to that console port (5000 control, 5001-5100
           data), stream ids chosen by the broker, never reused while open
  DATA  4  either way, bytes for that stream
  FIN   5  either way, no more bytes this way; a stream ends once FIN went both ways.
           The console sends FIN at once when its local connect fails.
  PING  6  either way, id 0; answered by PONG 7 with the same payload
A side that hears nothing for DEAD seconds drops the tunnel, every stream with it. The console
pings after PING_IDLE quiet seconds and redials with backoff (app/sources/tunnel.c).

PASV: ftp.c answers 227 / 229 with the console's own address and a port in 5001-5100. The
broker reads console-to-client control lines, opens a listener on its FTP address at the same
port (any free port when that one is taken), rewrites the reply to point there, and on the
client's connect sends OPEN for the console's port. Data streams are tunnel streams like
control; nothing on the console changes.

Public entry: Tailscale Funnel, `tailscale funnel --bg --tls-terminated-tcp=10000 tcp://127.0.0.1:8741`,
off by the same command plus `off`. Funnel ports 443, 8443, 10000 only; a Funnel port also binds
knave's tailnet address, so 443 would take every *.knave site from NPM and 8443 is atlas's fallback; needs MagicDNS, HTTPS certs
and the `funnel` nodeAttr; tailscaled terminates TLS with the knave.<tailnet>.ts.net certificate,
which the console checks against its embedded roots. Bandwidth is capped by Tailscale, unstated.
Anyone on the internet reaches the tunnel port: only an AUTH within 15 s keeps a connection.

TLS here (--tls): one SSL object, so every recv and send on it holds one lock; OpenSSL fails a
read and a write running at once. Client cipher order wins: the console lists ChaCha20 first.

Flow: the tunnel reader blocks on a slow client's send, which stalls every stream behind it;
FTP runs one transfer at a time per session, so that is the price of no window bookkeeping.
"""
import argparse
import hashlib
import hmac
import os
import re
import select
import socket
import ssl
import struct
import sys
import threading
import time

CHAL, AUTH, OPEN, DATA, FIN, PING, PONG = range(1, 8)
HDR = struct.Struct(">BHH")
MAXP = 16384
TAG = b"relay-tunnel 1"
DEAD = 75            # seconds without a frame before the tunnel is dropped; console pings at 25
PASV_WAIT = 20       # seconds a rewritten PASV listener waits for its client
P227 = re.compile(rb"^227 [^(]*\((\d+),(\d+),(\d+),(\d+),(\d+),(\d+)\)")
P229 = re.compile(rb"^229 [^(]*\(\|\|\|(\d+)\|\)")


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def addr(s):
    h, _, p = s.rpartition(":")
    return h, int(p)


def read_exact(sock, n):
    out = bytearray()
    while len(out) < n:
        b = sock.recv(n - len(out))
        if not b:
            raise ConnectionError("closed")
        out += b
    return bytes(out)


class Tunnel:
    """One authenticated console connection; streams live in self.streams by id."""

    def __init__(self, sock):
        self.sock = sock
        self.wlock = threading.Lock()
        self.slock = threading.Lock()
        self.streams = {}
        self.next_id = 1
        self.alive = True
        self.rbuf = bytearray()

    def read(self, n):
        """n bytes from the console. An SSL object takes no concurrent read and write (OpenSSL
        fails both with "A failure in the SSL library"), so recv runs under wlock, entered only
        once select or pending() says a record is there."""
        while len(self.rbuf) < n:
            with self.wlock:
                pending = getattr(self.sock, "pending", lambda: 0)()
            if not pending and not select.select([self.sock], [], [], DEAD)[0]:
                raise ConnectionError("console silent")
            with self.wlock:
                b = self.sock.recv(65536)
            if not b:
                raise ConnectionError("closed")
            self.rbuf += b
        out = bytes(self.rbuf[:n])
        del self.rbuf[:n]
        return out

    def send(self, t, sid, payload=b""):
        with self.wlock:
            if not self.alive:
                raise ConnectionError("tunnel down")
            for i in range(0, max(len(payload), 1), MAXP):
                part = payload[i:i + MAXP]
                self.sock.sendall(HDR.pack(t, sid, len(part)) + part)

    def open(self, client, port, control):
        with self.slock:
            while self.next_id in self.streams or self.next_id == 0:
                self.next_id = (self.next_id + 1) & 0xFFFF
            sid = self.next_id
            self.next_id = (self.next_id + 1) & 0xFFFF
            st = Stream(self, sid, client, control)
            self.streams[sid] = st
        self.send(OPEN, sid, struct.pack(">H", port))
        threading.Thread(target=st.pump, daemon=True).start()
        return st

    def drop(self, sid):
        with self.slock:
            self.streams.pop(sid, None)

    def run(self):
        """Reads frames until the console goes; every stream is closed after."""
        self.sock.settimeout(DEAD)
        try:
            while True:
                t, sid, n = HDR.unpack(self.read(HDR.size))
                body = self.read(n) if n else b""
                if t == PING:
                    self.send(PONG, 0, body)
                    continue
                if t == PONG:
                    continue
                st = self.streams.get(sid)
                if not st:
                    continue
                if t == DATA:
                    st.to_client(body)
                elif t == FIN:
                    st.console_fin()
        except (OSError, ConnectionError, struct.error) as e:
            log("tunnel down:", repr(e))
        finally:
            self.alive = False
            for st in list(self.streams.values()):
                st.kill()
            try:
                self.sock.close()
            except OSError:
                pass


class Stream:
    def __init__(self, tun, sid, client, control):
        self.tun, self.sid, self.client, self.control = tun, sid, client, control
        self.fin_in = self.fin_out = False
        self.line = b""
        self.lock = threading.Lock()

    def pump(self):
        """Client to console."""
        try:
            while True:
                b = self.client.recv(MAXP)
                if not b:
                    break
                self.tun.send(DATA, self.sid, b)
        except OSError as e:
            log("stream", self.sid, "ended:", repr(e))
        try:
            self.tun.send(FIN, self.sid)
        except (OSError, ConnectionError):
            pass
        with self.lock:
            self.fin_out = True
            self.maybe_done()

    def to_client(self, b):
        if self.control:
            self.line += b
            out = []
            while b"\n" in self.line:
                ln, self.line = self.line.split(b"\n", 1)
                out.append(self.rewrite(ln + b"\n"))
            b = b"".join(out)
        try:
            self.client.sendall(b)
        except OSError:
            pass

    def rewrite(self, ln):
        m = P227.match(ln)
        if m:
            port = int(m.group(5)) * 256 + int(m.group(6))
            lp = pasv_listen(self.tun, self.client, port)
            if lp is None:
                return b"425 Broker cannot open a passive port\r\n"
            h = self.client.getsockname()[0].replace(".", ",").encode()
            return b"227 Entering Passive Mode (%s,%d,%d)\r\n" % (h, lp >> 8, lp & 0xFF)
        m = P229.match(ln)
        if m:
            lp = pasv_listen(self.tun, self.client, int(m.group(1)))
            if lp is None:
                return b"425 Broker cannot open a passive port\r\n"
            return b"229 Entering Extended Passive Mode (|||%d|)\r\n" % lp
        return ln

    def console_fin(self):
        if self.line:
            try:
                self.client.sendall(self.line)
            except OSError:
                pass
            self.line = b""
        try:
            self.client.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        with self.lock:
            self.fin_in = True
            self.maybe_done()

    def maybe_done(self):
        if self.fin_in and self.fin_out:
            self.tun.drop(self.sid)
            try:
                self.client.close()
            except OSError:
                pass

    def kill(self):
        try:
            self.client.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.client.close()


def pasv_listen(tun, client, port):
    """Listener for one data connection on the client's side of the broker; returns its port."""
    host = client.getsockname()[0]
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    for p in (port, 0):
        try:
            ls.bind((host, p))
            break
        except OSError:
            continue
    else:
        ls.close()
        return None
    ls.listen(1)
    ls.settimeout(PASV_WAIT)

    def accept_one():
        try:
            c, _ = ls.accept()
        except OSError:
            return
        finally:
            ls.close()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            tun.open(c, port, control=False)
        except (OSError, ConnectionError):
            c.close()

    threading.Thread(target=accept_one, daemon=True).start()
    return ls.getsockname()[1]


class Broker:
    def __init__(self, secret, tls):
        self.secret = secret
        self.tls = tls
        self.tun = None
        self.lock = threading.Lock()

    def console(self, raw, peer):
        sock = raw
        try:
            raw.settimeout(15)
            if self.tls:
                sock = self.tls.wrap_socket(raw, server_side=True)
            nonce = os.urandom(32)
            sock.sendall(HDR.pack(CHAL, 0, 32) + nonce)
            t, _, n = HDR.unpack(read_exact(sock, HDR.size))
            mac = read_exact(sock, n) if n <= 64 else b""
            want = hmac.new(self.secret, nonce + TAG, hashlib.sha256).digest()
            if t != AUTH or not hmac.compare_digest(mac, want):
                log("tunnel refused from", peer[0])
                sock.close()
                return
        except (OSError, ConnectionError, ssl.SSLError, struct.error) as e:
            log("tunnel handshake failed from", peer[0], e)
            sock.close()
            return
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        tun = Tunnel(sock)
        with self.lock:
            old, self.tun = self.tun, tun
        if old:
            old.alive = False
            try:
                old.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        log("console up from", peer[0], *(sock.cipher() or ())[:2] if self.tls else ())
        tun.run()
        with self.lock:
            if self.tun is tun:
                self.tun = None
        log("console gone")

    def client(self, c, peer):
        with self.lock:
            tun = self.tun
        if not tun or not tun.alive:
            try:
                c.sendall(b"421 Console not connected to the broker\r\n")
            finally:
                c.close()
            return
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            tun.open(c, 5000, control=True)
        except (OSError, ConnectionError):
            c.close()


def serve(bind, handler):
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(bind)
    ls.listen(8)

    def loop():
        while True:
            c, peer = ls.accept()
            threading.Thread(target=handler, args=(c, peer), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--secret", required=True)
    ap.add_argument("--tunnel", default="127.0.0.1:8741")
    ap.add_argument("--ftp", default="127.0.0.1:5000")
    ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"))
    a = ap.parse_args()
    with open(a.secret, "rb") as f:
        secret = f.read().strip()
    if len(secret) < 16:
        sys.exit("secret shorter than 16 bytes")
    tls = None
    tun_bind = addr(a.tunnel)
    if a.tls:
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(*a.tls)
        tls.options &= ~ssl.OP_CIPHER_SERVER_PREFERENCE   # the console lists ChaCha20 first, no AES unit
    elif tun_bind[0] not in ("127.0.0.1", "localhost"):
        sys.exit("plain tunnel port must bind loopback; pass --tls to listen elsewhere")
    b = Broker(secret, tls)
    serve(tun_bind, b.console)
    serve(addr(a.ftp), b.client)
    log("tunnel on", a.tunnel, "tls" if tls else "plain", "| ftp on", a.ftp)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
