#!/bin/bash
# Everything the RISC OS alignment rig checks, after a LINUX_ARM_TEST=1 build:
#   the three projects' own assembly test suites (checkasm), each covering
#   every asm function they have, then tests/qemu/run.sh (decoders, encoders,
#   swscale, swresample through the ffmpeg command).
# Usage: QEMU=path/to/patched/qemu-arm tests/qemu/run-all.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
S=$(cd "$HERE/../.." && pwd)/src-linuxarm
: "${QEMU:?set QEMU to the patched qemu-arm (tests/qemu/build-qemu.sh)}"
export QEMU
bad=0
step() { echo "== $1"; shift; "$@" || { echo "FAILED: $*"; bad=1; }; }
step "FFmpeg checkasm" "$HERE/aligntrap.sh" "$S/ffmpeg-5.1.10/tests/checkasm/checkasm"
step "dav1d checkasm" "$HERE/aligntrap.sh" "$S/dav1d-1.5.4/build-ro/tests/checkasm"
step "x264 checkasm (8 bit)" "$HERE/aligntrap.sh" "$S/x264-master/checkasm8"
step "x264 checkasm (10 bit)" "$HERE/aligntrap.sh" "$S/x264-master/checkasm10"
[ -d "$HERE/samples" ] || "$HERE/make-samples.sh"
step "ffmpeg jobs" "$HERE/run.sh"
[ $bad -eq 0 ] && echo "ALL PASSED" || echo "SOME FAILED"
exit $bad
