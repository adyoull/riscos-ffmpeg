#!/bin/sh
# check-unixlib.sh ELF... - each program was linked with UnixLib 5.0.1 or
# later (the thread timer that can run from the PThreadTicker module).
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
for f in "$@"; do
  if ! ${CROSS}nm "$f" 2>/dev/null | grep -q ' __pthread_call_every_code$'; then
    echo "$f: not linked with a UnixLib that has the ticker fix" >&2; bad=1; continue
  fi
  size=$(${CROSS}objdump -d "$f" | awk '/<no_dynamic_area>:$/{p=1;next} p&&/^$/{exit}
    p&&/mov\tr3, #/{s=$0} p&&/svc\t0x0002001e/{sub(/.*#/,"",s); sub(/[ \t;].*/,"",s); print s; exit}')
  if [ "$size" = "$EXPECTED" ]; then
    echo "  $(basename "$f"): UnixLib 5.0.1 (the $EXPECTED-byte pthread block)"
  else
    echo "$f: claims a ${size:-?}-byte pthread block, not $EXPECTED: relink with UnixLib 5.0.1" >&2; bad=1
  fi
done
exit $bad
