#!/bin/bash
# hwhevc/module/build.sh [OUTDIR] - builds the HEVCHW relocatable module
# (HEVCHW,ffa). Freestanding C and the header in assembler, linked at 0
# with the header first, turned into a flat binary. A module is loaded
# anywhere in the RMA, so the build refuses anything that would need
# relocating: absolute-address relocations or a GOT.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
# (the Linux ARM compiler: GCCSDK's reaches even its own strings through a
# GOT when position independent; this code uses no library either way)
CROSS=${CROSS:-arm-linux-gnueabihf-}
mkdir -p "$OUT"
CFLAGS="-O2 -marm -march=armv7-a -mfloat-abi=hard -mfpu=vfpv3 -mgeneral-regs-only
  -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fPIE -fvisibility=hidden
  -fno-stack-protector -fno-common -fno-unwind-tables -fno-asynchronous-unwind-tables
  -fno-plt -mno-unaligned-access -Wall -Wextra -Werror"
${CROSS}gcc $CFLAGS -c -o "$OUT/hevchw.o" "$HERE/hevchw.c"
${CROSS}gcc -march=armv7-a -mfloat-abi=hard -c -o "$OUT/header.o" "$HERE/header.s"
cat > "$OUT/module.ld" <<'EOF'
ENTRY(module_base)
SECTIONS
{
  . = 0;
  .text : { *(.text.header) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) *(.bss .bss.* COMMON) . = ALIGN(4); }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) *(.ARM.exidx*) }
}
EOF
${CROSS}ld -nostdlib --emit-relocs -T "$OUT/module.ld" -o "$OUT/hevchw.elf" "$OUT/header.o" "$OUT/hevchw.o"
# position independence: only PC-relative relocations may appear
BAD=$(${CROSS}readelf -rW "$OUT/hevchw.elf" | grep -E 'R_ARM_(ABS|GOT|BASE|TARGET1|GLOB|JUMP|RELATIVE)' || true)
if [ -n "$BAD" ]; then
  echo "hwhevc: relocations a module can't have:"; echo "$BAD"; exit 1
fi
SECS=$(${CROSS}readelf -SW "$OUT/hevchw.elf" | grep -oE '\] \.[a-z.]+' | sed 's/] //' | grep -vE '^\.(text|symtab|strtab|shstrtab|rel\.text)$' || true)
if [ -n "$SECS" ]; then
  echo "hwhevc: unexpected sections: $SECS"; exit 1
fi
${CROSS}objcopy -O binary "$OUT/hevchw.elf" "$OUT/HEVCHW,ffa"
# (the .bss must be in the image: it's zeros at the end, objcopy writes them
# because it's inside .text)
SIZE=$(stat -c %s "$OUT/HEVCHW,ffa")
END=$(${CROSS}readelf -sW "$OUT/hevchw.elf" | awk '$8=="module_base"{print "ok"}')
[ "$END" = ok ] || { echo "hwhevc: no module_base"; exit 1; }
FIRST=$(od -An -tx4 -N4 "$OUT/HEVCHW,ffa" | tr -d ' ')
[ "$FIRST" = 00000000 ] || { echo "hwhevc: the header isn't first"; exit 1; }
echo "hwhevc: $OUT/HEVCHW,ffa ($SIZE bytes)"
