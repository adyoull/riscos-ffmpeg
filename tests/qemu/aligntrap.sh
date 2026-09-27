#!/bin/bash
# aligntrap.sh PROGRAM [ARGS...]
# Runs an arm-linux program under the patched qemu-arm ($QEMU) with RISC OS's
# alignment rules (SCTLR.A = 1) applied to the program's own code only:
# glibc and ld.so, which are not part of what runs on RISC OS, are exempt.
# Exit status 135 (SIGBUS) = an access that would abort on RISC OS.
set -u
prog=$1; shift
end=$(arm-linux-gnueabihf-readelf -lW "$prog" | awk '$1=="LOAD" && / R E / {print $3, $6; exit}')
lo=$(( ${end% *} )); hi=$(( ${end% *} + ${end#* } ))
export QEMU_ARM_ALIGN_TRAP=1
export QEMU_ARM_ALIGN_IGNORE=$(printf '0-%x,%x-ffffffff' $lo $hi)
export QEMU_LD_PREFIX=${QEMU_LD_PREFIX:-/usr/arm-linux-gnueabihf}
# QEMU_ARGS: extra qemu options, e.g. "-g 1234" to wait for gdb
exec "${QEMU:-qemu-arm}" ${QEMU_ARGS:-} "$prog" "$@"
