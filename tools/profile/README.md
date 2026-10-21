# Instruction profiles

Where the instructions go in riscos-ffmpeg's code, per function, by running
the arm-linux build under QEMU with a plugin that counts every block of
code it runs. Not timings (memory and cache costs don't show), but where to
start looking, and a check that a change cut what it was meant to. On a
Pi 4, a million instructions is very roughly half a millisecond.

1. `make test-build` (FFmpeg and its libraries for arm-linux).
2. `QEMU_PLUGINS=1 tests/qemu/build-qemu.sh` (QEMU 8.2.2 with RISC OS's
   alignment traps and TCG plugins, in `~/qemu-aligntrap`).
3. Then:

```
tools/profile/run.sh reel FILM overlay            # reelcore playback into YV12 (the VideoOverlay path)
tools/profile/run.sh reel FILM sprite 1440 810    # into 32bpp (the sprite and ReelEGL path)
tools/profile/run.sh reel FILM overlay -panel -subs FILE.srt -sound
tools/profile/run.sh ffmpeg -i FILM -vf scale=-2:480 -f null -
```

`prof_reel` plays the file through reelcore as Reel does, drawing every
picture (the tests' fake clock: nothing is ever late or skipped), with the
VideoCore and HEVC block off: everything on the ARM. `TOP_N` sets how many
functions are listed; `PROF_OUT` keeps the raw counts (one file each when
running several at once). `symsum.py BINARY RAW` lists them again.

Real footage profiles differently from test patterns: the clips for the
2026-10-21 survey were two seconds of a camera film, made 1080p and 720p
with x264, x265 and libvpx.
