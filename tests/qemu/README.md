# The alignment test rig

RISC OS traps unaligned accesses; Linux doesn't. This rig runs the same
FFmpeg (same sources, patches and NEON rewrite, built for
`arm-linux-gnueabihf` with `-mno-unaligned-access -DFF_STRICT_ALIGN`)
under a QEMU that traps like RISC OS, so an access that would abort on the
Pi stops the program here, with gdb showing where.

1. `tests/qemu/build-qemu.sh` builds `qemu-arm`: QEMU 8.2.2 plus
   `qemu-8.2.2-align-trap.patch`. It needs libglib2.0-dev, ninja and
   python3-venv.
   - `QEMU_ARM_ALIGN_TRAP=1` sets SCTLR.A at start-up.
   - `QEMU_ARM_ALIGN_IGNORE=lo-hi,...` exempts code ranges.
     `aligntrap.sh` exempts everything outside the program's own text,
     i.e. glibc and ld.so, whose string functions rely on unaligned
     loads.
2. Build the test copy. It goes in `stage-linuxarm/` and `src-linuxarm/`,
   and builds the checkasm programs of dav1d and x264 too:
   `LINUX_ARM_TEST=1 build/build-deps.sh ogg vorbis lame opus x264 dav1d`,
   then `LINUX_ARM_TEST=1 build/build-ffmpeg.sh`, then
   `make -C src-linuxarm/ffmpeg-5.1.10 tests/checkasm/checkasm`.
   It needs gcc-arm-linux-gnueabihf and qemu's armhf sysroot
   (libc6-dev-armhf-cross).
3. `tests/qemu/make-samples.sh`: short clips in about 25 codecs. It uses
   the host's own ffmpeg with x265, vpx, aom and theora.
4. `QEMU=… tests/qemu/run-all.sh`: the three checkasm suites, then
   `run.sh`.
   - `run.sh` runs each job with NEON and with `-cpuflags 0` (plain C),
     and compares the framemd5 output.
   - `run.sh PATTERN` runs only the jobs whose names match.

gdb-multiarch is used to report where a fault happened.
