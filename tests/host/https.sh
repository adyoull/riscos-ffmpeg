#!/bin/bash
# https through the AcornSSL backend (FFmpeg patch 0018), on Linux: the
# arm-linux test build of ffmpeg/ffprobe has tests/host/fake_acornssl.c
# linked in, a pass-through stand-in for the module (no encryption), so
# "https://" is served by a plain HTTP server here. Checks FFmpeg's side:
# the calls AcornSSL gets and their order, the handshake loop, reading a
# whole file, the host name (SNI), errors, a missing module, a timeout.
# Run by tests/host/run.sh; needs python3 and the trapping qemu.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
FF=$TOP/src-linuxarm/ffmpeg-5.1.10
SAMPLES=$TOP/tests/qemu/samples
RUN="$TOP/tests/qemu/aligntrap.sh"
TMP=$(mktemp -d)
LOG=$TMP/acornssl.log
export FAKE_ACORNSSL_LOG=$LOG
bad=0
ok()   { echo "  ok: $1"; }
fail() { echo "FAIL: $1"; bad=1; }

# a plain HTTP server for the samples, and one that never answers
PORT=$((20000 + RANDOM % 20000)); STALL=$((PORT + 1))
python3 "$HERE/httpserve.py" "$PORT" "$SAMPLES" >/dev/null 2>&1 &
S1=$!
python3 "$HERE/httpserve.py" "$STALL" --silent >/dev/null 2>&1 &
S2=$!
trap 'kill $S1 $S2 2>/dev/null; rm -rf "$TMP"' EXIT
sleep 1
CLIP=h264_aac_640_360.mp4

# 1. a numeric address: the session, no host name, non-blocking, closed with the socket still open
: > "$LOG"
if "$RUN" "$FF/ffprobe_g" -v error -show_entries stream=codec_name -of csv=p=0 "https://127.0.0.1:$PORT/$CLIP" > "$TMP/p1" 2>&1 &&
   grep -q h264 "$TMP/p1"; then ok "ffprobe https://127.0.0.1 ($(tr '\n' ' ' < "$TMP/p1"))"; else fail "ffprobe https://127.0.0.1: $(cat "$TMP/p1")"; fi
seq=$(tr '\n' ',' < "$LOG")
n=$(grep -c '^createsession' "$LOG"); nb=$(grep -cx 'fionbio 1' "$LOG"); nc=$(grep -cx 'close socket_open=1' "$LOG")
# every session (FFmpeg reconnects to seek): made non-blocking, and closed while its socket is still open
if [ "$n" -ge 1 ] && [ "$nb" = "$n" ] && [ "$nc" = "$n" ] && ! grep -q 'socket_open=0' "$LOG" &&
   [ "$(head -1 "$LOG" | cut -d' ' -f1)" = createsession ] && [ "$(sed -n 2p "$LOG")" = "fionbio 1" ]; then
  ok "$n sessions, each: createsession, fionbio 1, ..., close with the socket open"
else
  fail "calls: $seq"
fi
grep -q hostname "$LOG" && fail "a host name was set for a numeric address"

# 2. by name: SNI, and the whole file read through it is the same as the file
: > "$LOG"
want=$("$RUN" "$FF/ffmpeg_g" -v error -i "$SAMPLES/$CLIP" -map 0 -c copy -f md5 - 2>&1)
got=$("$RUN" "$FF/ffmpeg_g" -v error -i "https://localhost:$PORT/$CLIP" -map 0 -c copy -f md5 - 2>&1)
[ -n "$want" ] && [ "$got" = "$want" ] && ok "the whole file through https: $got" || fail "file through https: $got (want $want)"
grep -qx "hostname localhost" "$LOG" && ok "host name localhost (SNI)" || fail "no host name set: $(tr '\n' ',' < "$LOG")"

# 3. a long handshake (e.g. while the desktop asks about a certificate)
if FAKE_ACORNSSL_STEPS=60 "$RUN" "$FF/ffprobe_g" -v error -show_entries format=duration -of csv=p=0 "https://127.0.0.1:$PORT/$CLIP" > "$TMP/p3" 2>&1 &&
   grep -q '^[0-9]' "$TMP/p3"; then ok "60-step handshake"; else fail "60-step handshake: $(cat "$TMP/p3")"; fi

# 4. a failed handshake: AcornSSL's message, and the session closed
: > "$LOG"
if FAKE_ACORNSSL_FAIL=1 "$RUN" "$FF/ffprobe_g" -v error "https://127.0.0.1:$PORT/$CLIP" > "$TMP/p4" 2>&1; then
  fail "a failed handshake was accepted"
else
  grep -q "Handshake error (state -9984)" "$TMP/p4" && ok "failed handshake reported: $(grep -m1 Handshake "$TMP/p4")" ||
    fail "failed handshake: $(cat "$TMP/p4")"
  grep -q "close socket_open=1" "$LOG" && ok "session closed after the failure" || fail "not closed after the failure: $(tr '\n' ',' < "$LOG")"
fi

# 5. no AcornSSL module
if FAKE_ACORNSSL_ABSENT=1 "$RUN" "$FF/ffprobe_g" -v error "https://127.0.0.1:$PORT/$CLIP" > "$TMP/p5" 2>&1; then
  fail "https worked without the module"
else
  grep -q "need the AcornSSL module" "$TMP/p5" && ok "no module: $(grep -m1 -o 'https and.*later' "$TMP/p5")" || fail "no module: $(cat "$TMP/p5")"
fi

# 6. a server that never answers: the read times out (-rw_timeout 1 s), it doesn't hang
t0=$(date +%s)
if timeout 60 "$RUN" "$FF/ffprobe_g" -v error -rw_timeout 1000000 "https://127.0.0.1:$STALL/x.mp4" > "$TMP/p6" 2>&1; then
  fail "the silent server gave a file"
else
  t=$(( $(date +%s) - t0 ))
  [ $t -lt 30 ] && ok "silent server: gave up after ${t} s ($(head -c 80 "$TMP/p6" | tr '\n' ' '))" || fail "silent server: ${t} s"
fi

[ $bad = 0 ] && echo "all passed" || echo "FAILED"
exit $bad
