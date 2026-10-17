#!/bin/sh
# Run by .github/workflows/riscos-deps-update.yml as
#   ci/riscos-deps-hook.sh NAME OLD_TAG NEW_TAG
# from the top of the repository when NAME (a line of .riscos-deps) has a
# newer release, to move the pins kept outside .riscos-deps:
#   riscos-mesa         its devkit's file and sha256 in build/SOURCES and
#                       build/SHA256SUMS (env.sh and the tests read the
#                       version from .riscos-deps)
#   riscos-reelhwaccel  third_party/reelhwaccel/ from the release's devkit
#                       (headers, libraries, README, COPYING), its FFmpeg
#                       patches as our 0021, 0023 and 0027, and a line in
#                       SOURCE.md with the sha256
#   riscos-crossdev     nothing (the workflows' image follows .riscos-deps)
set -eu
name=$1 old=${2#v} new=${3#v}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fetch() {   # fetch URL FILE: prints the sha256
  curl -fsSL --retry 3 -o "$tmp/$2" "$1"
  sha256sum "$tmp/$2" | cut -d' ' -f1
}
case $name in
riscos-mesa)
  f_old=riscos-mesa-devkit-$old.tgz f_new=riscos-mesa-devkit-$new.tgz
  url=https://github.com/adyoull/riscos-mesa/releases/download/v$new/$f_new
  sum=$(fetch "$url" "$f_new")
  awk -v o="$f_old" -v n="$f_new" -v s="$sum" '$2 == o {print s "  " n; next} {print}' build/SHA256SUMS > "$tmp/sums"
  awk -v o="$f_old" -v n="$f_new" -v u="$url" '$1 == o {print n " " u; next} {print}' build/SOURCES > "$tmp/sources"
  grep -q " $f_new\$" "$tmp/sums" && grep -q "^$f_new " "$tmp/sources" ||
    { echo "riscos-deps-hook: no $f_old line in build/SHA256SUMS or build/SOURCES" >&2; exit 1; }
  cat "$tmp/sums" > build/SHA256SUMS
  cat "$tmp/sources" > build/SOURCES
  ;;
riscos-reelhwaccel)
  f=riscos-reelhwaccel-devkit-$new.tgz
  sum=$(fetch "https://github.com/adyoull/riscos-reelhwaccel/releases/download/v$new/$f" "$f")
  tar xzf "$tmp/$f" -C "$tmp"
  d=$tmp/riscos-reelhwaccel-devkit-$new
  T=third_party/reelhwaccel
  cp "$d"/include/*.h "$T/include/"
  cp "$d"/lib/*.a "$T/lib/"
  cp "$d/README.md" "$d/COPYING" "$T/"
  for m in "0001 0021" "0002 0023" "0003 0027"; do
    set -- $m
    src=$(ls "$d"/ffmpeg/$1-*.patch 2>/dev/null | head -1)
    dst=$(ls patches/ffmpeg/$2-*.patch 2>/dev/null | head -1)
    [ -n "$src" ] && [ -n "$dst" ] && cp "$src" "$dst"
  done
  {
    echo
    echo "- Devkit $new, \`$f\` (sha256 $sum), from the release v$new by"
    echo "  ci/riscos-deps-hook.sh: its headers, libraries, README and COPYING, and"
    echo "  its FFmpeg patches 0001-0003 as our 0021, 0023 and 0027 (check they"
    echo "  apply without fuzz, and that our 0022 still applies after 0021)."
  } >> "$T/SOURCE.md"
  ;;
esac
