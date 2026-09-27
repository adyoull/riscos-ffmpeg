#!/bin/sh
# neon-align-apply.sh LISTFILE [--check] file.S ...
# Runs tools/neon-align.py with the hand-checked exceptions in LISTFILE:
#   FILE:LINE         allowed as is (wide-element vld2/3/4 or macro-argument
#                     register list on data that is aligned to its elements)
#   whole FILE:LINE   macro-argument register list that is whole registers
# Line numbers are those of the source after the patches in patches/.
list=$1; shift
args=$(grep -v '^#' "$list" | grep . | while read -r a b; do
  if [ "$a" = whole ]; then echo "--whole $b"; else echo "--allow $a"; fi
done)
exec python3 "$(dirname "$0")/neon-align.py" $args "$@"
