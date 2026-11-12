#!/bin/sh
# Builds common/ftp.c for the Mac and drives it with lftp and curl against a temp root.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d); pid=
trap 'kill $pid 2>/dev/null; chmod -R u+rwX "$work"; rm -rf "$work"' EXIT
mkdir -p "$work/root/3ds/app" "$work/root/dir with space"
printf hello > "$work/root/3ds/app/a.txt"
head -c 3000000 /dev/urandom > "$work/big.bin"
cat > "$work/main.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "ftp.h"
static int site(char *arg, char *out, size_t cap) {
    if (strcasecmp(arg, "PING")) return 0;
    snprintf(out, cap, "pong\nsecond");
    return 200;
}
/* Sink stand-in: uploads under /install/ land in SUNK, renamed so the test can tell the paths apart. */
static int sopen(const char *abs, void **ctx, const char **err) {
    if (strncmp(abs, "/install/", 9)) return 0;
    if (strstr(abs, "bad")) { *err = "Refused by sink"; return -1; }
    return (*ctx = fopen(SUNK, "wb")) ? 1 : -1;
}
static int swrite(void *ctx, const void *p, size_t n) { return fwrite(p, 1, n, ctx) == n; }
static const char *sclose(void *ctx, int ok) { fclose(ctx); return ok ? NULL : "aborted"; }
static const ftp_hooks_t hooks = { site, sopen, swrite, sclose };
int main(void) { ftp_set_hooks(&hooks); ftp_log("boot"); ftp_start(0, "trey test"); pause(); }
C
cc -O1 -w -I"$here/inc" -I"$here/.." -DFTP_PORT=5099 -DSD_PREFIX="\"$work/root\"" -DSUNK="\"$work/sunk.bin\"" \
   "$here/../ftp.c" "$work/main.c" -o "$work/ftpd" -lpthread
"$work/ftpd" & pid=$!
sleep 0.5
U=ftp://127.0.0.1:5099
fail() { echo "FAIL: $*"; exit 1; }
curl -s "$U/" | grep -q ' 3ds$' || fail list root
curl -s "$U/3ds/app/a.txt" | grep -qx hello || fail retr
curl -s -T "$work/big.bin" "$U/up/big.bin" --ftp-create-dirs || fail stor
cmp "$work/big.bin" "$work/root/up/big.bin" || fail stor content
if ! { curl -s "$U/up/big.bin" -o "$work/back.bin" && cmp "$work/big.bin" "$work/back.bin"; }; then fail retr big; fi
curl -s "$U/dir%20with%20space/" >/dev/null || fail space dir
lftp -c "open $U; cls -l /; cd '/dir with space'; put $work/big.bin -o x.bin; mv x.bin y.bin; cls; rm y.bin; cd ..; mirror /3ds $work/mirror" >/dev/null || fail lftp
cmp "$work/mirror/app/a.txt" "$work/root/3ds/app/a.txt" || fail mirror
[ ! -e "$work/root/dir with space/y.bin" ] || fail rm
lftp -c "open $U; cd /up; rm big.bin; cd /; rmdir up" || fail rmdir
[ ! -e "$work/root/up" ] || fail rmdir gone
curl -s -Q 'CWD /nope' "$U/" >/dev/null 2>&1 && fail cwd missing
curl -s --ftp-method nocwd -T "$work/big.bin" "$U/install/x.cia" || fail sink
cmp "$work/big.bin" "$work/sunk.bin" || fail sink content
rm "$work/sunk.bin"
lftp -c "open $U; put $work/big.bin -o /install/y.cia" >/dev/null || fail sink lftp
cmp "$work/big.bin" "$work/sunk.bin" || fail sink lftp content
[ ! -e "$work/root/install" ] || fail sink wrote a file
curl -s --ftp-method nocwd -T "$work/big.bin" "$U/install/bad.cia" 2>/dev/null && fail sink refuse
site=$(curl -s -Q 'SITE PING' "$U/" -o /dev/null -v 2>&1)
echo "$site" | grep -q '< 200-pong' && echo "$site" | grep -q '< 200 End' || fail site multiline
log=$(curl -s -Q 'SITE LOG' "$U/" -o /dev/null -v 2>&1)
echo "$log" | grep -q 'boot' && echo "$log" | grep -q 'install /install/x.cia' || fail site log
curl -s -Q 'SITE NOPE' "$U/" >/dev/null 2>&1 && fail site unknown
echo ok
