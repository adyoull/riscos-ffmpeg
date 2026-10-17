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
  if [ ! -f "$DL/$file" ]; then
    url=$(awk -v f="$file" '$1 == f {print $2}' "$TOP/build/SOURCES")
    [ -n "$url" ] || { echo "no URL for $file in build/SOURCES" >&2; bad=1; continue; }
    echo "fetching $file"
    curl -fsSL --retry 3 -o "$DL/$file.part" "$url" && mv "$DL/$file.part" "$DL/$file" ||
      { rm -f "$DL/$file.part"; echo "can't fetch $file from $url" >&2; bad=1; continue; }
  fi
  if ! echo "$sum  $DL/$file" | sha256sum -c --quiet - 2>/dev/null; then
    echo "checksum failed: $file (removed)" >&2
    rm -f "$DL/$file"
    bad=1
  fi
done < "$TOP/build/SHA256SUMS"
[ $bad = 0 ] && echo "sources: all $(grep -c . "$TOP/build/SHA256SUMS") present and checked"
exit $bad
