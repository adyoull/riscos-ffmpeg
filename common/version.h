/*
 * version.h - the versions shown in the apps' Info windows.
 *
 * The one place the apps take their version numbers from. When releasing,
 * change the number and date here and in the places listed below; the host
 * tests (tests/host/run.sh, "versions agree") check they all match:
 *   FFMPEG_APP_VERSION  Makefile VERSION (as 5.1.10-riscosN),
 *                       build/package.sh's default, !FFmpeg's !Help header
 *   REEL_VERSION        build/package.sh's REEL_VERSION default,
 *                       !Reel's and !ReelEGL's !Help headers
 * Dates are written the RISC OS way (dd-Mmm-yyyy), as other apps' Info
 * windows show them.
 */
#ifndef RISCOS_FFMPEG_VERSION_H
#define RISCOS_FFMPEG_VERSION_H

#define FFMPEG_APP_VERSION  "5.1.10-riscos16"
#define FFMPEG_APP_DATE     "02-Oct-2026"

#define REEL_VERSION        "0.1.23"
#define REEL_DATE           "02-Oct-2026"

#define APP_AUTHOR          "Andrew Youll"

#endif
