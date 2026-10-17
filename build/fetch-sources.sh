#!/usr/bin/env bash
# make sources: every file build/SHA256SUMS names, in dl/. Those missing are
# downloaded from build/SOURCES; then every one is checked against its
# sha256 (a wrong download is removed, so the next try fetches it again).
set -euo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
DL=${DL:-$TOP/dl}
mkdir -p "$DL"
bad=0
while read -r sum file; do
  [ -n "$file" ] || continue
  if [ -f "$DL/$file" ] && ! echo "$sum  $DL/$file" | sha256sum -c --quiet - >/dev/null 2>&1; then
    echo "checksum failed: $file in dl/ (fetching it again)" >&2
    rm -f "$DL/$file"
  fi
  if [ ! -f "$DL/$file" ]; then
    urls=$(awk -v f="$file" '$1 == f {$1 = ""; print}' "$TOP/build/SOURCES")
    [ -n "$urls" ] || { echo "no URL for $file in build/SOURCES" >&2; bad=1; continue; }
    for url in $urls; do
      echo "fetching $file from $url"
      if curl -fsSL --retry 3 -o "$DL/$file.part" "$url" &&
         echo "$sum  $DL/$file.part" | sha256sum -c --quiet - >/dev/null 2>&1; then
        mv "$DL/$file.part" "$DL/$file"
        break
      fi
      echo "  not there, or the wrong checksum" >&2
      rm -f "$DL/$file.part"
    done
    [ -f "$DL/$file" ] || { echo "can't fetch $file with its checksum" >&2; bad=1; }
  fi
done < "$TOP/build/SHA256SUMS"
[ $bad = 0 ] && echo "sources: all $(grep -c . "$TOP/build/SHA256SUMS") present and checked"
exit $bad
