#!/bin/sh
# check-versions.sh - the version numbers agree everywhere.
#
# common/version.h is what the apps' Info windows show; the Makefile,
# build/package.sh, build/build-ffmpeg.sh (ffmpeg -version) and the !Help
# headers carry the same numbers. Run by tests/host/run.sh; exits 1 and says
# which file disagrees.
TOP=$(cd "$(dirname "$0")/.." && pwd)
V=$(sed -n 's/^#define FFMPEG_APP_VERSION *"\(.*\)"/\1/p' "$TOP/common/version.h")
R=$(sed -n 's/^#define REEL_VERSION *"\(.*\)"/\1/p' "$TOP/common/version.h")
N=${V#5.1.10-}
bad=0
want() { # file, text wanted
  grep -qF -- "$2" "$1" || { echo "  $1 doesn't have \"$2\" (common/version.h)"; bad=1; }
}
want "$TOP/Makefile" "VERSION ?= $V"
want "$TOP/build/package.sh" "V=\${1:-$V}"
want "$TOP/build/package.sh" "RV=\${REEL_VERSION:-$R}"
want "$TOP/build/build-ffmpeg.sh" "--extra-version=$N"
head -1 "$TOP/app/!FFmpeg/!Help,fff" | grep -q " $N\$" || { echo "  !FFmpeg's !Help header isn't $N"; bad=1; }
for a in Reel ReelEGL; do
  head -1 "$TOP/app/!$a/!Help,fff" | grep -q " $R\$" || { echo "  !$a's !Help header isn't $R"; bad=1; }
done
# the PThreadTicker module the apps carry is the one released with UnixLib 5.0.1
( cd "$TOP/third_party/pthreadticker" && sha256sum -c --quiet PThrTicker.sha256 ) ||
  { echo "  third_party/pthreadticker/PThrTicker isn't the released module"; bad=1; }
[ $bad = 0 ] && echo "  FFmpeg $V, Reel $R: all agree"
exit $bad
