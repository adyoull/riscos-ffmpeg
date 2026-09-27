# riscos-ffmpeg. See README.md. GCCSDK_ENV, DEVKIT, JOBS: see build/env.sh.
VERSION ?= 5.1.10-riscos4
.PHONY: help sources deps ffmpeg ffegl package test-build test clean
help:
	@echo "make sources | deps | ffmpeg | ffegl | package [VERSION=...] | test-build | test (QEMU=...) | clean"
sources:
	cd dl && sha256sum -c ../build/SHA256SUMS
deps:
	build/build-deps.sh
ffmpeg:
	build/build-ffmpeg.sh
ffegl:
	build/build-ffegl.sh
package:
	build/package.sh $(VERSION)
test-build:
	LINUX_ARM_TEST=1 build/build-deps.sh ogg vorbis lame opus x264 dav1d
	LINUX_ARM_TEST=1 build/build-ffmpeg.sh
	$(MAKE) -C src-linuxarm/ffmpeg-5.1.10 tests/checkasm/checkasm
test:
	tests/qemu/run-all.sh
clean:
	rm -rf src stage src-linuxarm stage-linuxarm dist tests/qemu/out
