#!/bin/bash
# tools/elf-align-data.py (tools/unixlib-overlay.sh's alignment of UnixLib's
# assembler .data, as crossdev's ".balign 4" does), on objects assembled
# here like UnixLib's: .data started without an alignment.
#   - words at multiples of 4: the section is aligned to 4 after, and the
#     linker then puts it at a multiple of 4 after an odd-sized one;
#   - a symbol at .data+1: refused, the object unchanged;
#   - an empty .data, and one aligned already: left alone;
#   - the same in a library (.a), changed in place, a byte for each (after
#     a member of odd size, not an object).
set -uo pipefail
TOP=$(cd "$(dirname "$0")/../.." && pwd)
AS=arm-linux-gnueabihf-as LD=arm-linux-gnueabihf-ld READELF=arm-linux-gnueabihf-readelf NM=arm-linux-gnueabihf-nm
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
align() { $READELF -SW "$1" | awk '/\] \.data / { print $NF }'; }

printf '.data\n.global w1\nw1: .word 1\n.global w2\nw2: .word 2\n' | $AS -o "$T/words.o"
printf '.data\n.byte 1\n.global odd\nodd: .word 2\n' | $AS -o "$T/odd.o"
printf '.data\n.text\nnop\n' | $AS -o "$T/empty.o"
printf '.data\n.balign 4\n.word 3\n' | $AS -o "$T/aligned.o"
printf '.data\n.global b\nb: .byte 7\n' | $AS -o "$T/byte.o"         # (an odd-sized .data before)
mkdir "$T/a" && cp "$T/words.o" "$T/empty.o" "$T/a/" && cp "$T/odd.o" "$T/a/odd.o"   # (for the libraries)
[ "$(align "$T/words.o")" = 1 ] || fail "the assembler aligned words.o's .data already: the test proves nothing"

out=$(python3 "$TOP/tools/elf-align-data.py" "$T/words.o" "$T/empty.o" "$T/aligned.o")
[ $? = 0 ] || fail "words.o, empty.o, aligned.o: exit status not 0"
[ "$(align "$T/words.o")" = 4 ] || fail "words.o: .data aligned to $(align "$T/words.o"), not 4"
[ "$(align "$T/empty.o")" = 1 ] || fail "empty.o: its empty .data was changed"
[ "$out" = "$T/words.o: .data aligned to 1, now 4" ] || fail "said: $out"
$LD -o "$T/linked" -e 0 "$T/byte.o" "$T/words.o" 2>/dev/null
w1=$($NM "$T/linked" | awk '$3 == "w1" { print $1 }')
[ -n "$w1" ] && [ $((0x$w1 % 4)) = 0 ] || fail "linked after a 1-byte .data: w1 at 0x$w1, not a multiple of 4"

cp "$T/odd.o" "$T/odd.before"
python3 "$TOP/tools/elf-align-data.py" "$T/odd.o" 2>"$T/err" && fail "odd.o (a symbol at .data+1): not refused"
grep -q "a symbol at .data+1" "$T/err" || fail "odd.o: said \"$(cat "$T/err")\""
cmp -s "$T/odd.o" "$T/odd.before" || fail "odd.o: changed, though refused"

# a library: the objects in it fixed in place, 4 bytes for each section; a
# library with an object that can't be, left as it was
arm-linux-gnueabihf-ar rc "$T/lib.a" "$T/a/words.o" "$T/a/odd.o" "$T/a/empty.o"
printf 'odd' > "$T/a/note"                                   # (an odd size: padded in the library)
arm-linux-gnueabihf-ar rc "$T/good.a" "$T/a/note" "$T/a/empty.o" "$T/a/words.o"
cp "$T/good.a" "$T/good.before"; cp "$T/lib.a" "$T/lib.before"
out=$(python3 "$TOP/tools/elf-align-data.py" "$T/good.a") || fail "good.a: exit status not 0"
n=$(cmp -l "$T/good.before" "$T/good.a" | wc -l)
[ "$n" = 1 ] || fail "good.a: $n bytes changed, not 1 (sh_addralign 1 -> 4)"
[ "$out" = "$T/good.a(words.o): .data aligned to 1, now 4" ] || fail "good.a: said: $out"
(cd "$T/a" && arm-linux-gnueabihf-ar x ../good.a words.o)
[ "$(align "$T/a/words.o")" = 4 ] || fail "good.a: words.o's .data aligned to $(align "$T/a/words.o")"
python3 "$TOP/tools/elf-align-data.py" "$T/lib.a" 2>/dev/null && fail "lib.a (with odd.o): not refused"
cmp -s "$T/lib.a" "$T/lib.before" || fail "lib.a: changed, though refused"

[ $fails = 0 ] && echo "elf_align_test: all passed" || echo "elf_align_test: $fails FAILED"
exit $((fails > 0))
