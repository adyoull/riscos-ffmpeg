# riscos-ffmpeg. See README.md. GCCSDK_ENV (or RISCOS_TOOLCHAIN), DEVKIT, JOBS: see build/env.sh.
VERSION ?= 5.1.10-riscos24
.PHONY: help sources deps ffmpeg apps ffegl package check test-build test clean
help:
	@echo "make sources | deps | ffmpeg | apps | package [VERSION=...] | check (RCD_DIR=...) | test-build | test (QEMU=...) | clean"
sources:
	build/fetch-sources.sh
deps:
	build/build-deps.sh
ffmpeg:
	build/build-ffmpeg.sh
apps:
	build/build-apps.sh

ffegl: apps
package:
	build/package.sh $(VERSION)
check:
	ci/check-programs.sh
test-build:
	LINUX_ARM_TEST=1 build/build-deps.sh ogg vorbis lame opus x264 dav1d
	LINUX_ARM_TEST=1 build/build-ffmpeg.sh
	$(MAKE) -C src-linuxarm/ffmpeg-5.1.10 tests/checkasm/checkasm
test:
	tests/qemu/run-all.sh
clean:
	rm -rf src stage src-linuxarm stage-linuxarm dist tests/qemu/out
