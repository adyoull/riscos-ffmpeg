#!/bin/sh
# check-unixlib.sh ELF... - the toolchain's library is UnixLib 5.0.3.1-rc8, and
# each program was linked with UnixLib 5.0.1 or later (the thread timer that can run from the PThreadTicker module), and
# a program with FFmpeg's file protocol with UnixLib 5.0.2's files over
# 2GB: its libavformat built against 5.0.2's headers calls
# __unixlib_fstat64 (the 64-bit st_size), not the old fstat64.
#
# UnixLib 5.0.1's start-up code claims a 472-byte pthread block in the RMA
# (the ticker code and its counters); the Warzone toolchain's earlier
# UnixLib claimed 248, GCCSDK's own 120. A program linked with the wrong
# library would still run, but without the fixes the apps' !Run files load
# PThreadTicker for. Same test as riscos-unixlib's tools/check-lib.sh:
# the "mov r3, #<size>" before OS_Module 6 in no_dynamic_area.
# Run by build/package.sh on every program it packages.
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
EXPECTED=${EXPECTED:-472}
bad=0
# UnixLib 5.0.3.1-rc8 (fork in EABI programs, _exit(n), mmap offsets, the
# rc fixes since 5.0.3) can't be told from 5.0.2/5.0.3 by a program's
# symbols: check the library the programs were just linked with is that
# release's (pre-release v5.0.3.1-rc8; the same library as rc7).
UNIXLIB_SHA256=${UNIXLIB_SHA256:-44cb5481ec4f1c6784ec81b2d2ddd6f2de72b063bcde2b13725a9443b1de8078}
LIB=$(${CROSS}gcc -print-file-name=libunixlib.a 2>/dev/null)
if [ -f "$LIB" ] && [ "$(sha256sum "$LIB" | cut -d' ' -f1)" != "$UNIXLIB_SHA256" ]; then
  echo "$LIB: not UnixLib 5.0.3.1-rc8's libunixlib.a (sha256 ${UNIXLIB_SHA256%${UNIXLIB_SHA256#????????}}...)" >&2; bad=1
elif [ -f "$LIB" ]; then
  echo "  the toolchain's libunixlib.a: UnixLib 5.0.3.1-rc8"
fi
for f in "$@"; do
  if ! ${CROSS}nm "$f" 2>/dev/null | grep -q ' __pthread_call_every_code$'; then
    echo "$f: not linked with a UnixLib that has the ticker fix" >&2; bad=1; continue
  fi
  size=$(${CROSS}objdump -d "$f" | awk '/<no_dynamic_area>:$/{p=1;next} p&&/^$/{exit}
    p&&/mov\tr3, #/{s=$0} p&&/svc\t0x0002001e/{sub(/.*#/,"",s); sub(/[ \t;].*/,"",s); print s; exit}')
  syms=$(${CROSS}nm "$f" 2>/dev/null)
  if echo "$syms" | grep -q ' ff_file_protocol$' && ! echo "$syms" | grep -q ' __unixlib_fstat64$'; then
    echo "$f: FFmpeg's file protocol without UnixLib 5.0.2's large files: rebuild libavformat against 5.0.2" >&2; bad=1; continue
  fi
  if [ "$size" = "$EXPECTED" ]; then
    echo "  $(basename "$f"): UnixLib 5.0.2 or later (the $EXPECTED-byte pthread block$(echo "$syms" | grep -q ' __unixlib_fstat64$' && echo ', files over 2GB'))"
  else
    echo "$f: claims a ${size:-?}-byte pthread block, not $EXPECTED: relink with UnixLib 5.0.2" >&2; bad=1
  fi
done
exit $bad
