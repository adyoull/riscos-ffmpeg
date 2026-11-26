#!/bin/sh
# check-unixlib.sh ELF... - the toolchain's library is riscos-crossdev 1.14's
# UnixLib 5.0.3.12, and
# each program was linked with UnixLib 5.0.1 or later (the thread timer that can run from the PThreadTicker module), and
# a program with FFmpeg's file protocol with UnixLib 5.0.2's files over
# 2GB: its libavformat built against 5.0.2's headers calls
# __unixlib_fstat64 (the 64-bit st_size), not the old fstat64.
#
# UnixLib 5.0.3.1's start-up code claims a 640-byte pthread block in the RMA
# (the ticker code and its counters; 5.0.1 to 5.0.3.1-rc9 claimed 472, the
# Warzone toolchain's earlier UnixLib 248, GCCSDK's own 120). A program linked with the wrong
# library would still run, but without the fixes the apps' !Run files load
# PThreadTicker for. Same test as riscos-unixlib's tools/check-lib.sh:
# the "mov r3, #<size>" before OS_Module 6 in no_dynamic_area.
# Run by build/package.sh on every program it packages.
CROSS=${CROSS:-/root/gccsdk/env/bin/arm-riscos-gnueabihf-}
EXPECTED=${EXPECTED:-640}
bad=0
# UnixLib's fixes can't be told from 5.0.3.1 by a program's symbols, so
# check the library the programs were just linked with is the one wanted:
# riscos-crossdev toolchain 1.14's, UnixLib 5.0.3.12 (the same code as
# riscos-unixlib's v5.0.3.12 release; its assembler data word-aligned in
# UnixLib's own sources now). The container image the GitHub workflows
# build in has it. 5.0.3.11/12 over 5.0.3.10: __thread one per thread, no
# deaths when memcpy or setjmp first touch a stack page, faster string
# functions, sysconf(_SC_NPROCESSORS_ONLN) 1 (or UnixLib$Processors).
UNIXLIB_SHA256=${UNIXLIB_SHA256:-df399f0418bf4406eda8c9d935fb67f1206ef67db300f3ffd300fd982d225e07}
LIB=$(${CROSS}gcc -print-file-name=libunixlib.a 2>/dev/null)
if [ -f "$LIB" ] && [ "$(sha256sum "$LIB" | cut -d' ' -f1)" != "$UNIXLIB_SHA256" ]; then
  echo "$LIB: not riscos-crossdev 1.14's libunixlib.a, UnixLib 5.0.3.12 (sha256 df399f04...)" >&2; bad=1
elif [ -f "$LIB" ]; then
  echo "  the toolchain's libunixlib.a: UnixLib 5.0.3.12 (riscos-crossdev 1.14)"
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
    echo "  $(basename "$f"): UnixLib 5.0.3.x (the $EXPECTED-byte pthread block$(echo "$syms" | grep -q ' __unixlib_fstat64$' && echo ', files over 2GB'))"
  else
    echo "$f: claims a ${size:-?}-byte pthread block, not $EXPECTED: relink with UnixLib 5.0.3.12" >&2; bad=1
  fi
done
exit $bad
