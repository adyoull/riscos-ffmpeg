# Changes

Reel's versions were renumbered 0.1.1–0.1.9 (they were 0.1–0.9; the
tags are `reel-0.1.1` … `reel-0.1.9`).

## Unreleased (next FFmpeg and Reel): H.264 on the Pi's VideoCore, long web addresses, File types as Iris does it

- **h264_vchiq: H.264 decoded by the Raspberry Pi's VideoCore** (patch
  0021, from riscos-reelhwaccel's devkit, with its vcdec library). Built
  in when `third_party/reelhwaccel` has the devkit (`--enable-vchiq`);
  FFmpeg is built as before without it. Not FFmpeg's default H.264
  decoder: `-c:v h264_vchiq` (ffplay `-vcodec h264_vchiq`). Up to
  1920x1088, Baseline/Main/High 8-bit 4:2:0; 1080p needs gpu_mem=128.
- **Reel and ReelEGL use the VideoCore for H.264 by themselves** (reelcore:
  `open_video_decoder`): h264_vchiq first, FFmpeg's h264 on the ARM when
  it refuses the stream (or isn't built in); if it fails part way
  (AVERROR_EXTERNAL), the ARM's decoder takes over at the next update,
  from the same place (a seek there). `REELCORE_NO_VIDEOCORE`, from
  `*Set Reel$NoVideoCore 1` (ReelEGL$NoVideoCore), keeps to the ARM.
  Frame skipping and automatic deblocking-off don't apply while the
  VideoCore decodes (it decodes every frame). Media info: "Decoder
  VideoCore (h264_vchiq)", or "h264, 1 thread (the VideoCore failed part
  way)".
- **No log in release builds**: `!Run` sets `Reel$Log` (`ReelEGL$Log`)
  to `off` unless it's already set, as the log costs a little time while
  playing. Test builds (a version with a suffix) come with that line
  commented out by `build/package.sh`, so they log as before. To log with
  a release build, put a `|` in front of the line.
- **1080p60 on the VideoCore: late pictures no longer copied** (seen on a
  Pi 4: 10 of 60 pictures a second shown, the ARM never idle). The
  VideoCore decodes every picture; what each costs the ARM is the copy
  out of its memory (about 14 ms at 1080p), and the 50 a second that
  came too late were copied and then thrown away. Patch 0022 gives
  h264_vchiq a `drop_before` option: pictures before it go back to the
  VideoCore uncopied. reelcore sets it two pictures' time behind the
  clock, and to the seek point on the way to a seek's picture.
  vc_test: a slow machine (60 ms a picture at 25 fps) with and without
  bursts, and a seek: every picture accounted for (shown, late or
  dropped), the decoder dropping some; none dropped while keeping up.
- **Fixed: H.264 on the VideoCore stuttered and jumped** (seen on a Pi 4
  with the first test build). The VideoCore hands pictures back in
  bursts (around big keyframes, and at the start), and reelcore, which
  keeps at most 12 ready, pushed the oldest out of its queue without a
  word: the picture jumped ahead. Now it takes a decoder's pictures only
  while there's room, leaves the rest in the decoder and keeps the packet
  it couldn't send yet; a picture pushed out is logged (it shouldn't
  happen). vc_test: a stand-in that gives bursts of 16 and refuses
  packets meanwhile, with a seek: every picture shown, no jumps (before
  the fix: 90 of 150 shown, 8 jumps).
- **Reel shows which decoder is at work while it plays**: the picture's
  stats (**S**) have a Decoder row, "VideoCore (hardware)", "ARM
  (software)" or "ARM (software: the VideoCore failed part way)"; Media
  info's Decoded row ends "(VideoCore)" or "(ARM)"; and so does each
  second's line in Reel's log. reelcore: `ReelCoreStats.decoder`
  (REELCORE_DECODER_ARM, _VIDEOCORE, _ARM_AFTER).
- **reelcore no longer loses a packet a decoder refuses for now**
  (`avcodec_send_packet` EAGAIN, as h264_vchiq gives while the
  VideoCore's input is full): it takes the decoder's frames and sends the
  same packet again. FFmpeg's own decoders never refused, so nothing
  changes for them.
- vc_test (host): a stand-in h264_vchiq (FFmpeg's h264 with its calls
  wrapped): every packet refused once and not a picture lost; refused at
  open; REELCORE_NO_VIDEOCORE; failing at the 40th picture and playing
  on to the end on the ARM. Mutations (no resend, no fallback) caught.
  The stats' decoder is checked too (VideoCore while it decodes, then
  ARM after the failure), and reel_test checks the panel's Decoder row.
- Built with riscos-reelhwaccel devkit 0.1 (sha256 7cfcea8c…) in
  `third_party/reelhwaccel`.


- **File types: Never / While Reel is running / Always** (Chris on the
  ROOL forum, after Iris's choice for PDF files). The ticks now say which
  types Reel opens: unticked, it doesn't claim them at all (0.1.22 claimed
  every video type while running, ticked or not). While running: it
  claims ticked types' double-clicks (Message_DataOpen) and sets no Run
  aliases. Always: it also sets the ticked types' `Alias$@RunType_XXX`
  (and Data's RunData) and writes them to `Choices:Reel.Types` for !Boot;
  choosing another takes Reel's aliases off again and gives Data's back
  to the program that had it. The choice and ticks are kept in
  `Choices:Reel.Choices` (`open_mode`, `open_types`, `open_data`). With
  none saved yet, types whose Run alias is already Reel's mean Always
  with those ticked; otherwise While running with all ticked, as before.
- **A second copy started with a file hands it to the running one**
  (Message_DataLoad to its icon bar icon) instead of quitting with it:
  a ticked type's Run alias pointing at Reel did nothing while Reel was
  running and hadn't claimed the double-click (Chris: "nothing happens").
- The File types menu's title was cut short ("Double-click opens" is
  longer than a menu title can be): it's "File types".
- reel_test: the default sets no aliases; Always sets them and the Types
  file and keeps the other program's Data alias; unticking AVI removes
  its alias and an AVI double-click isn't claimed; While running takes
  every alias off and gives Data's back; Never claims nothing; a file from
  a second copy is taken; and a second copy started with a file sends it
  to the running one. Mutations caught.

- **Open address takes addresses up to 8191 characters** (it held 1023
  and cut longer ones off without a word: Raik on the ROOL forum). A
  googlevideo address from yt-dlp often passes 1500 characters, and
  `-g` gives two. Pasted text, dropped files and playlists had no limit;
  only the field and its copy for saved HLS playlists did.
- reel_test pastes a 3000-character address (a googlevideo-like query
  ending "END") and plays it, and run.sh checks the whole of it reached
  the test server. With the old limit: "pasted 1023 of 3042 characters".
- **tools/eabiprobe** (not in the downloads; `EABIProbe-0.1.zip` for
  testers): Raik found Iris won't start while Reel runs. ARMEABISupport
  can't be replaced while any GCCSDK program uses it ("There are still
  clients using ARMEABISupport"), so a !Run asking for a newer one fails
  then. EABIProbe reports the loaded ARMEABISupport, SharedUnixLibrary,
  PThreadTicker, VFPSupport and DDEUtils, the copies in System:Modules,
  `*ARMEABISupport_Info`'s list of programs, the tasks, and Iris's !Run
  lines and modules, then says whether any copy is newer than the one
  loaded. Host test: Raik's suspected machine (1.02 loaded, 1.05 in
  !Iris), all 1.05, an old one loaded at boot, none loaded, no Iris.

## Unreleased: ReelHWAccel has its own project

The hardware video decoding work (`reelhwaccel/`, the HEVCHW module, and
the test tools `tools/hevcprobe`, `vchiqprobe`, `mmalprobe` and
`mmaldecode` with their host tests) moved, with its history, to
riscos-reelhwaccel (github.com/adyoull/riscos-reelhwaccel). FFmpeg and
Reel will use its decoders through its devkit, as they use riscos-mesa's.

## 5.1.10-riscos15 (2026-10-01): UnixLib 5.0.3.1

FFmpeg's own code is unchanged since riscos14; the programs are relinked
with the UnixLib releases since then (riscos14 had 5.0.2).


- **Relinked with UnixLib 5.0.3.1** (github.com/adyoull/riscos-unixlib
  release v5.0.3.1, an unofficial fork of GCCSDK's UnixLib;
  `libunixlib.a` sha256 fa98152f…). Since rc8: heaps past 128 MB, and
  threads that run in programs that poll often (UnixLib's thread timer
  keeps running through Wimp_Poll, so ffplay's and SDL's threads get time
  however often a program polls), the fixes of rc9. No interface or header
  changes, so the programs were relinked and FFmpeg's libraries weren't
  rebuilt. 5.0.3.1's start-up claims a 640-byte pthread block (472 before):
  `tools/check-unixlib.sh` checks for that and for 5.0.3.1's library.
- **PThreadTicker 0.03** (from the same release) replaces 0.02 in the
  apps: it holds 5.0.3.1's new timer code (programs built with 5.0.3.1
  use 0.03 only, older ones 0.01/0.02 only; either runs its own copy if
  it doesn't find one it uses). The !Run files still ask for 0.01, as the
  module's ReadMe advises.
- Before that, the unreleased test builds were linked with UnixLib 5.0.3
  and 5.0.3.1-rc8; their notes follow, as they were.
- Not in the downloads: `tools/hevcprobe`, `vchiqprobe`, `mmalprobe`,
  `mmaldecode` and `reelhwaccel/` (the HEVCHW module) - the first steps
  towards hardware video decoding on the Raspberry Pi (ReelHWAccel), each
  with a host test.

### UnixLib 5.0.3.1-rc8 (test builds)

- **Relinked with UnixLib 5.0.3.1-rc8** (github.com/adyoull/riscos-unixlib
  pre-release v5.0.3.1-rc8; `libunixlib.a` sha256 44cb5481…, the same
  library as rc7). Since 5.0.3: `fork()` works in EABI programs (a fork
  child exiting no longer frees its parent's stack), `_exit(n)` exits with
  code n, `mmap()` offsets, and the review fixes of the rc series. The
  installed headers are unchanged (sched.h differs by a comment only), so
  FFmpeg's libraries weren't rebuilt; the programs were relinked.
  `tools/check-unixlib.sh` checks for rc8's library.
- **PThreadTicker 0.02** (from the same pre-release) replaces 0.01 in the
  apps: it updates its count of programs with interrupts off. The !Run
  files still ask for 0.01, as the module's ReadMe advises (a loaded,
  in-use older copy can't be replaced).

### UnixLib 5.0.3 (test builds)

- **Relinked with UnixLib 5.0.3** (github.com/adyoull/riscos-unixlib
  v5.0.3; `libunixlib.a` sha256 761305fa…). Its fixes: `read()` into a
  buffer on the stack could kill a program ("Fatal signal received: EMT
  trap") when the SWI wrote to a stack page not yet mapped; `ctime()`,
  `asctime()` and their `_r` forms could return a bad pointer; no build
  paths in the library's debug information. Same symbols and installed
  headers as 5.0.2, so FFmpeg's libraries weren't rebuilt, only the
  programs relinked. `tools/check-unixlib.sh` (run by package.sh) now
  also checks the toolchain's libunixlib.a is 5.0.3's, since a program
  linked with it can't be told from one linked with 5.0.2.

## Reel 0.1.22 (2026-10-01): the pointer hides full screen, a sound track key, file types, saved HLS playlists

Three requests from Chris on the ROOL forum:

- **The pointer hides full screen** after 2 seconds with the mouse left
  alone, and comes back as soon as it moves or a button is pressed
  (`OS_Byte 106`; the shape before is put back). Checked on every null;
  while it's hidden and paused, nulls come a few times a second so a
  move is still seen. Shown again on leaving full screen, before an
  error box and on quitting. `*Set Reel$NoHidePointer 1` keeps it.
- **#** chooses the next sound track (files with more than one; after
  the last, the first again), as the Sound track menu does.
- **File types** on the icon bar menu: the video file types MimeMap knows
  (from avi, mp4, m4v, mkv, webm, mov, mpg, ts, vob, wmv, flv, ogv, 3gp;
  each type once), ticked when a double-click of that type starts Reel
  (its `Alias$@RunType_XXX` is Reel's). Choosing one sets or clears the
  RunType now and writes `Choices:Reel.Types`, an Obey file !Boot runs, so
  it lasts. While Reel is loaded it still claims any video double-clicked
  (Message_DataOpen), ticked or not.
- **Data files named like a video** (film/mp4: MP4, MKV and WebM have no
  settled RISC OS filetype, and files from other computers arrive as
  Data): while Reel is loaded it claims a double-clicked Data file whose
  name ends in a video extension. File types > "Data, by name" sets
  `Alias$@RunType_FFD` to `Obey <Reel$Dir>.RunData %*0`, a new Obey file
  that picks the extension with `SetEval … RIGHT n` and starts Reel, or
  passes any other Data file to the RunType there before (kept in
  `Alias$ReelOldRunData`; given back when unticked). Pi report: an AVI
  ticked worked, MP4 didn't.
- reel_test: full screen, the pointer hidden after 2 s left alone, back
  when the mouse moves, hidden again, back on leaving full screen; # with
  a two-track file (second, then first); File types > AVI ticked (the
  RunType and the Types file) and unticked. Mutations caught for each.
- **A saved HLS playlist with relative names** (Raik on the ROOL forum: a
  master .m3u8 saved from a site names its streams "hlsv.m3u8", relative
  to where it came from): dropped on Reel, it opens Open address asking
  "The address this playlist came from". Given that, Reel writes
  `<Wimp$ScrapDir>.ReelHLS` with each stream and segment name (and the
  `URI="..."` of `#EXT-X-MEDIA` and similar lines) made whole from it -
  `//host/x`, `/x` and `x` as a browser would, the address's own query
  left off - and plays that, titled by the dropped file. Text that isn't
  an http(s) address is refused with a message; Escape or Close forgets
  the playlist and puts the window's usual label back. Playlists that
  already name whole addresses play as before.
- reel_test: a saved master naming "hlsv.m3u8" dropped, the window asks,
  "not an address" refused, `<server>/live/master.m3u8?session=1` given:
  the scrap file names `<server>/live/hlsv.m3u8` and it plays (322x184).
  sources_test: what counts as relative, and the three kinds of join.
- Linked with UnixLib 5.0.3.1 and carrying PThreadTicker 0.03, as
  riscos15 (below).

## 5.1.10-riscos14 (2026-09-30): MP4 files from the Convert window, properly this time

- **Forum report (Raik):** reducing a 720p video to 480p with the Convert
  window's default settings made an unusable file, while his own command
  line worked. The window's command is fine; the fault was in patch 0020
  (riscos13), which every MP4/M4A with `-movflags +faststart` goes
  through on RISC OS. It closed and reopened the output's AVIOContext
  while movenc kept its old pointer and wrote the index (the moov)
  through freed memory. That worked only when the allocator handed the
  same block back; otherwise the file had no index ("moov atom not
  found") or ffmpeg crashed.
- **Patch 0020, reworked:** the AVIOContext stays the same object; only
  the file under it is closed, the data shifted in place through one
  read-write opening, and the trailer written through that opening. FLV
  (`-flvflags add_keyframe_index`) used the same path and is fixed too.
- **tests/qemu/faststart.sh** repeats every in-place run with an
  allocator that never reuses freed memory (`tests/qemu/nofree.c`): the
  old patch crashes there for MP4, MOV, M4A and FLV; the new one gives
  files byte-identical to the usual way. FLV added.
- **Convert window:** when a conversion fails, the unfinished file is
  deleted (as after Stop), so a broken file isn't left to be mistaken
  for a video. The status line says so.

## 5.1.10-riscos13 (2026-09-29): files over 2GB, MP4 files from the Convert window, and more options

- **Files over 2GB** (up to 4GB-1, the RISC OS limit): linked with
  **UnixLib 5.0.2** (github.com/adyoull/riscos-unixlib v5.0.2, tested on a
  Pi with a 3GB file). FFmpeg's configure already builds with
  `-D_FILE_OFFSET_BITS=64`; libavformat was rebuilt from clean against
  5.0.2's headers (`sys/stat.h`, `sys/mman.h`, `unistd.h`), so its file
  protocol seeks past 2GB (`lseek64`) and sees the 64-bit size
  (`__unixlib_fstat64`). Up to riscos12, UnixLib 5.0.1 stopped at 2GB-1
  (EOVERFLOW). build-apps.sh gives Reel, the front end and the examples
  `-D_FILE_OFFSET_BITS=64` too. `tools/check-unixlib.sh` now also checks
  that every program with FFmpeg's file protocol calls `__unixlib_fstat64`.

- **Forum report:** converting big_buck_bunny_480p.mpg to MP4 (the Convert
  window adds `-movflags +faststart`) failed at the end: "Unable to re-open
  … output file for shifting data" and "This file is already open"
  (&108C2). To move the index to the front, FFmpeg opens the output a
  second time for reading while it is still open for writing, and RISC OS
  refuses that.
- **Patch 0020** (libavformat/mux_utils.c, `ff_format_shift_data`): on RISC
  OS the output is closed, the data shifted along in place through one
  read-write opening (from the end backwards, 256 KB at a time: the file
  protocol's largest write; `truncate=0`), then opened again for writing
  without truncating, at the end of the shifted data. Elsewhere only when
  the second opening fails, or with `FFMPEG_SHIFT_IN_PLACE` set (tests).
- **Convert window: an Options field** for more ffmpeg options, as on the
  command line (e.g. `-vf hflip`, `-r 25`, `-metadata title=…`). They go
  after the window's own output options, before `-progress`, so they win.
  It must start with an option, be one line, and not add `-i`. Tab moves
  to it; greyed out while converting, like Trim. The window is 56 OS units
  taller. convert_test: the Obey file carries them; the three refusals.
- **tests/qemu/faststart.sh** (in run-all.sh): MP4 (5 chunks), MOV and
  M4A made in place are byte for byte what the usual way makes. Mutations
  caught: opening with truncation (an empty file), 1 MB writes (EIO).

## Reel 0.1.21 (2026-09-30): subtitles, chapters, frame steps, turned videos, 4K, and keeping up

- **Subtitles, drawn into the picture** (so they show over the hardware
  overlay), in reelcore:
  - Tracks are the file's subtitle streams and subtitle files:
    `reelcore_subtitle_tracks`/`_track_name`/`_track`,
    `reelcore_set_subtitle_track`, `reelcore_add_subtitle_file`,
    `reelcore_show_subtitles`, `reelcore_subtitle_text`. A stream marked
    default or forced is shown at the start (as mpv).
  - The chosen stream's packets are read with the rest (`pkt_kind` 2) and
    decoded as they come. Choosing it again seeks to the picture shown. A
    file is read whole.
  - Text (SubRip, ASS/SSA, mov_text, WebVTT): ASS override blocks
    dropped, \N and \h made plain, UTF-8 to Latin-1 (curly quotes,
    dashes and ellipses made plain). It's drawn white with a round black
    outline, centred near the bottom, wrapped to 92% of the width. The
    font is DejaVu Sans Bold at 22, 32 or 46 px, chosen for about 5% of
    the height shown (`reelcore/sub_font.h`, made by
    `tools/mksubfont.py`).
  - Pictures (DVD, PGS, DVB): palette to premultiplied RGBA, placed on
    their canvas scaled to the picture; an empty one ends the one before.
  - Events are kept in order of start, not doubled after a seek. Text is
    kept (up to 8192 events); only the last 8 pictures are kept.
- **Layers:** the stats panel and the subtitle share one "layer" code:
  placed per draw with a scale, nearest pixel, blended in
  `reelcore_draw_pixels` and in `reelcore_draw_yuv420`'s row copies.
  `reelcore_set_yuv_scale()` (Reel sets it from the overlay's size) makes
  both the size they'd be in the window. The panel's margin now scales
  with it too.
- **Chapters:** `reelcore_chapters`, `_chapter_start` (as
  `reelcore_position`), `_chapter_title`, `_chapter_at`. Reel has a
  Chapters menu (the time and the title of each), and Page Down / Page Up
  (the next; back to this one's start, or the one before within 2 s).
  Media info says how many.
- **Frame steps:** `reelcore_step` (paused: the next picture) and
  `reelcore_step_back` (a seek to 1.5 pictures before, exact). Playing
  again after a step seeks to the picture shown, so the sound starts
  there. In Reel: `.` and `,`, which pause first. `paused_show()` shows
  the picture after a seek or step while paused (up to 5 s of decoding).
  Choosing a chapter while paused uses it too.
- **Stepping back is quicker** (Pi report: frame stepping slow):
  - The pictures before are kept (up to 10, `hist`): the ones decoded on
    the way to a step back's seek (`bstep`), and each picture a step
    forward leaves. The next steps back take one at once
    (`reelcore_step_back` returns REELCORE_NEW_FRAME); only every 10th
    decodes from the key frame.
  - Every seek (not just steps) skips decoding non-reference pictures
    (most B-frames) more than 0.5 s before its picture
    (`skip_frame = AVDISCARD_NONREF`), restored when it's reached. From
    8 s into a clip with its key frame at 0: 116 pictures decoded instead
    of 203.
  - Reel shows the hourglass while it decodes for more than 0.3 s, up to
    20 s.
- **The sound kept to its queue when the video falls far behind** (the
  same 4K log: "StreamManager_AddBlock refused a block: Buffer full",
  15 s queued, then "the sound device isn't playing; carrying on without
  sound"). With the video at key frames only, reading raced ahead to find
  them, and every sound packet read was decoded and queued at once. Sound
  packets now wait as packets (`apk`, like the video's `vpk`) and are
  decoded only while less than `ahead` is queued; the decoder's flush at
  the end comes after them. slow_test has a heavy run (0.4 s a picture,
  key frames every second): at most 0.27 s queued (0.65 s before).
- **4K through the hardware overlay** (Pi report: a 3996x1730 trailer
  kept blanking the screen, and didn't with Hardware acceleration off;
  the log said "only 2 of 3 buffers (GPU memory)"):
  - Reel makes the overlay at most about 1920x1088's pixels (and 2048
    each way): bigger videos are halved (4K to 1998x864). That's a
    quarter of the GPU memory and of the display's scaling work.
  - `reelcore_draw_yuv420` takes a size of half the frame or less: yuv420p
    is halved row by row with NEON (`reelcore_halve_plane`), and 4:4:4
    has luma halved once and colour twice, all NEON (swscale's
    conversion took 176 ms a picture at 3996x1730). Other formats go
    through swscale scaling. Subtitles and the stats follow the overlay's
    size.
  - panel_test: halved YV12 is the 2x2 averages; 4:4:4 halved has luma
    equal to the averages and colour within 1. A mutation (4:4:4 through
    swscale) is caught.
- **Turned videos:** a stream's display matrix (phone videos) turns the
  picture 90/180/270 degrees through the same filter graph as yadif
  (transpose, or hflip+vflip). The size is swapped for 90/270. Media info
  has a Turned line. `REELCORE_NO_ROTATE` turns it off.
- **Reel:**
  - Subtitle files beside a local video (film/srt, /ass, /ssa, /vtt;
    Unix film.srt) are added when it opens. A subtitle file dropped on
    Reel is added to what's playing.
  - V hides or shows the subtitles; J goes to the next track (then none).
  - A Subtitles menu (None, then the tracks).
  - Media info has a Subtitles section.
- **Tests:**
  - New `sub_test`: MKV with a default SubRip track and chapters, an .srt
    added to an MP4, an ASS file, YV12 at 1x and 2x, frame steps, a
    display matrix of 90 degrees.
  - reel_test phases: a .srt dropped on the window (the text in the
    picture), V twice, J, `.` `.` `,` and Space.
  - panel_test's margin scales with the overlay.
  - Mutations caught: no rotation, no override-tag stripping, no layers
    in YV12.

- **Keeping up: deblocking off by itself.** Before any frames are skipped,
  reelcore turns off the H.264 deblocking filter (about a quarter of the
  decoding) when pictures take more than 0.9 of the time between them, or
  the video is 0.1 s behind with decoding above 0.75 of that time. It is
  tried on again every 10 s while decoding keeps up (after 20, 40 … 160 s
  when a try had to go straight off again), and comes back for good below
  0.6. The timers use real time (the clock goes back at a loop or seek).
  Media info's Frame skipping row and a "Keeping Up" row on the stats say
  when; `*Set Reel$NoAutoFast 1` (`REELCORE_NO_AUTOFAST`) stops it. On the
  Pi a 1080p 4:4:4 or high-bitrate 1080p file gains most; a 4K trailer
  still skips frames. slow_test: a video a little too slow keeps up with
  no skipping (5 skip spells without); a stall on an easy video, and a
  video just too slow, are tried and settle correctly.
- **Stats and subtitles readable full screen through the overlay.** They
  were drawn at the screen's size and shrunk into the frame a pixel at a
  time (a 720p frame on 1920x1200: 0.67), which dropped whole rows and
  columns of each letter. Now each is drawn in a font of the size it is
  seen, in the frame's own pixels, with no scaling: the panel from DejaVu
  Sans Mono Bold at 9-33 px (bigger on screens over 900 lines: about 18 px
  at 1200), subtitles from DejaVu Sans Bold at 16-56 px (5.2% of the
  frame's height). panel_test at scales 0.5, 1 and 2; sub_test: the same
  text height shown 1x and 2x. Confirmed on a Pi.

## Reel 0.1.20 (2026-09-29): files over 2GB, a steady clock, 60 fps, and stats on the picture

- **Files over 2GB** play (up to 4GB-1): linked with UnixLib 5.0.2 and
  FFmpeg riscos13's libraries (see riscos13). sources.c no longer takes
  ftell's -1 as a length for a text file it can't measure.

- **reelcore's `clock_smooth()` is now a timer steered by the sound**
  (0.1.19 snapped to each of the sound's readings). The clock is the
  system timer.
  - Each time the sound's reading moves, where the sound really is is
    taken as the reading plus half the time since the last look (the step
    came somewhere in between).
  - The clock moves a quarter of the way there, and jumps if they're more
    than 100 ms apart.
  - As before, it never runs more than 1.5 steps past the last reading and
    never goes backwards.

  Pictures come evenly, and a sound device running at a slightly different
  rate from the timer is followed.
- **`ReelCoreStats`:** `pace_sum`/`pace_n` (how evenly pictures are handed
  out) and `sync_err_sum`/`sync_err_n` (the clock against the sound at
  each step). Media info shows both: "evenly to N ms" on Pictures shown,
  and "steered to within N ms of it" on Clock. !Help has a "Keeping time"
  section.
- **The overlay no longer waits for the vsync.** A picture that comes
  before the overlay's last switch has happened used to wait for the
  vsync there and then (OS_Byte 19), up to 16.7 ms with nothing decoded
  meanwhile; at 60 fps that was most of the drawing time. Now it waits
  as `S.ov_pending`, and Reel carries on decoding and tries again on the
  next pass (without sleeping). If a newer picture is due first, the
  newer one is shown. Media info's Drawing line and the log count both
  ("N waited, N replaced").
- **Media info opens beside the player** (right, then left, else below or
  above) where there's room, so it doesn't cover the picture and hide the
  overlay. Otherwise it goes where it always did. `info_open` also had a
  block of 8 words for Wimp_GetWindowState, which writes 9: fixed.
- **Stats on the picture** (S, or the window menu): YouTube's "Stats for
  nerds" rows (Video / Source, Viewport / Frames, Current Res / Drawn,
  Volume, Codecs, Color, Connection Speed, Network Activity and Buffer
  Health with a graph of the last minute each, Timing, Date), drawn *into*
  the picture by reelcore, so it shows over the Pi's hardware overlay
  (which covers anything on the screen).
  - `reelcore_set_panel()` / `reelcore_panel_size()`: rows of label and
    value, optional graphs. `reelcore_draw_pixels` blends it 1:1 at the
    picture's top left; `reelcore_draw_yuv420` blends it into the YV12
    rows as they're copied (in cached memory, then written once), scaled
    by `yuv_scale` to undo the overlay's scaling.
  - The font is DejaVu Sans Mono Bold at 15 px, as 8-bit coverage
    (`reelcore/panel_font.h`, made by `tools/mkpanelfont.py`; Latin-1).
    Its licence is in `third_party/dejavu/Licence` and the zips' Licences.
  - **Tests:** `panel_test` (RGB both byte orders, YV12 at 1x and 2x,
    nothing changed outside the panel, graph bars, off again exactly as
    before). reel_test: S on, the text in the picture and the picture
    as reelcore draws it; S off.
- **Stats on the picture, for a file:** Connection Speed, Network Activity
  and Buffer Health only for a web address. For a file, Reading (Mbit/s
  from the file) and Ready (pictures decoded ahead, sound queued) instead,
  without graphs. Codecs and the container use the short names ("h264
  (High) / aac (LC)", "QuickTime / MOV"). reel_test checks the rows for a
  file and for a web address.
- **No more late pictures from slow ones at 60 fps** (a Pi 4 ReelEGL log of
  test60_720p_60s: 1–8 late a second in stretches, decoding 2x real time on
  average).
  - reelcore skipped a picture as late whenever the next one was due too.
    A picture that took longer than 16.7 ms to decode left two due at
    once, so one was thrown away. Now a picture is skipped only if the next
    one was due more than a picture's time ago (at most 40 ms); otherwise
    both are shown, one straight after the other.
  - `fill()` decoded up to 8 pictures in a row before a due one was shown
    (to fill up after a skip), which made the next ones late. It now stops
    when a picture is due.
  - Decoding ahead is by time: 0.13 s of pictures, 3 to 8 (8 at 60 fps, 4
    at 30, 3 at 24/25; it was always 3). QMAX 12.
- **Media info's "N in all"** late count starts again when the file is
  played again from the end, or you seek to the start.
- **clock_test:** a 60 fps clip with decoding taking time (6 ms, and 8
  pictures a second at 24 ms): 0 late, up to 8 decoded ahead. Mutations
  caught: the old skip rule (6–7 late), decoding in a row (20–32 late),
  3 ahead.
- **reel_test:** a new phase `P_OVLWAIT` holds the vsync counter still.
  Pictures wait without blocking or sleeping, none are drawn another way,
  and one is shown once the vsync comes. The fake overlay counts two
  switches in one vsync (tearing). Media info must not overlap the player.
  Mutations caught: the blocking wait put back, sleeping while a picture
  waits, and Media info opening in the old place.
- **clock_test:** sound reported in 2048-frame blocks, a 30 fps clip at
  44.1 kHz.
  - Pictures evenly spaced to 0.4 ms, the sound followed to within 0.5 ms,
    including with the sound hardware 1% fast.
  - Sleeping as Reel does: 3.7 ms, 3.6 ms.
  - Mutations caught: the raw clock (29 late, 16 ms uneven), and the timer
    without steering (25 ms drift with the sound 1% fast).

## Reel 0.1.19 (2026-09-29): 30 fps video shows all 30 pictures

- **The fix:** 30 fps video with 44.1 kHz sound showed only 21–26 pictures
  a second on a Pi 4, with 5–9 skipped as "late", although decoding kept
  up easily (2.2x real time) and Reel slept 40% of the time. The same
  happened in Reel and ReelEGL, with and without the overlay.
- **The cause:** the sound is the clock, and StreamManager reports what
  has been played a whole block (2048 sample frames) at a time. At
  44.1 kHz that's a 46 ms step, but a 30 fps picture is due every 33 ms.
  At each step the next picture was already due too, so the one before it
  was thrown away as late. (25 fps at 48 kHz lost fewer: 23.8 of 25 shown
  in earlier logs.)
- **reelcore:** `clock_smooth()` runs the sound clock on by itself
  between steps from where the last step put it. It never goes more than
  1.5 steps past the last reading (if the sound stops, so does the clock)
  and never backwards. It starts again after a seek, a resume or a speed
  change. Detecting a stalled device still uses the raw reading.
- **Tests:** `tests/host/clock_test.c`.
  - The fake sound device reports playback in 2048-frame blocks
    (`FAKE_AUDIO_BLOCK`), with a 30 fps, 44.1 kHz clip.
  - Now: 149 pictures in 5 s, none late.
  - Without the smoothing: 123 pictures and 25 late, the Pi's numbers.

## 5.1.10-riscos12 (2026-09-29): RISC OS names in the current directory

- **`ffmpeg -i holiday/mp4 small/mp4` now means the files holiday/mp4 and
  small/mp4 in the current directory**, as in the rest of RISC OS. UnixLib
  reads a name with no filing system, disc or directory prefix as a Unix
  one, so before this it opened the file "mp4" in a directory "holiday".
  (A forum report of ffmpeg "doing nothing" on the command line, with
  names written the way !Help showed them.)
  - FFmpeg patch 0019 (`libavformat/riscos_filename.h`, `file.c`):
    `ff_riscos_relative_name()` recognises these names. The rules: one
    `/`; no `.` after it; no prefix (a filing system, `:`, `$ @ ^ % & < \`);
    and after the `/`, an extension of a format FFmpeg knows (from any
    muxer or demuxer). Such a name is opened as `@.holiday/mp4`, which
    UnixLib leaves alone.
  - `Films.holiday/mp4` works the same way. Unix names (`clip.mp4`,
    `dir/clip.mp4`, `videos/clip`) and full RISC OS names are unchanged.
  - It applies to opening, checking, deleting and renaming files, in
    ffmpeg, ffprobe and ffplay alike.
- !Help: how names are read, with examples of each kind, and when to
  start a name with `@.`.
- Tests: `tests/host/riscos_name_test.c` (24 names, host gcc with the
  sanitizers).

## Reel 0.1.18 (2026-09-28): hardware acceleration (video overlays)

- **Hardware acceleration** on the window menu (on by default, saved in
  Choices as `hardware_acceleration`): with the VideoOverlay module, each
  picture is copied as YV12 (the decoder's yuv420p planes, row by row;
  other formats through swscale at the same size) into a hardware overlay,
  and the display scales it and converts it to RGB. No swscale conversion,
  scaling or plotting per picture. Both !Reel and !ReelEGL talk to
  VideoOverlay themselves. riscos-mesa's EGL overlays are RGB, 1:1, for
  visible-area surfaces; ReelEGL's picture is a work area surface.
- Built from riscos-mesa's Pi 4 measurements (ovltest):
  - Create is used as the probe (Vet fails on the Pi).
  - 3 buffers, else 2 when the GPU is short of memory.
  - It waits for a vsync when none has passed since the last switch (no
    tearing).
  - Overlay memory is only ever written (it is uncached: reads are slow).
  - After a mode change the old overlay is destroyed and a new one made.
- The Pi's overlays are "Basic" (over everything). The overlay is hidden,
  and the picture drawn as before:
  - while a window or menu overlaps the picture (Reel walks the window
    stack up from its window, at every picture);
  - while paused and at the end;
  - around error boxes and the "carry on" question.
- **Fallback:** whenever there's no overlay, the picture is drawn exactly
  as before, with no message:
  - the option off, `Reel$NoOverlay`, or `EGL$Overlay off`;
  - no module;
  - Create refused;
  - a buffer that can't be mapped;
  - a size outside the scaling limits;
  - any SWI error (not tried again until the size, colours or mode change).
- The menu item is shaded without the module and says "(in use)" when it
  is. Media info's Converting and Drawing lines say what's happening.
  `!Run` loads VideoOverlay if it's in !System (quietly).
- reelcore: `reelcore_draw_yuv420()` (planar 4:2:0 at the frame's size,
  write-only row copies; `REELCORE_YUV_709` / `_FULL` for the ModeFlags).
- Tests: reel_test (and the SharedSoundBuffer and EGL builds) with a fake
  VideoOverlay. The steps: Create refused (drawn as before, not retried),
  a mode change bringing it back, the selector (YV12, ModeFlags &6000,
  3 buffers), pictures going through the overlay and not the sprite, the
  buffer equal to the frame's YV12, Fit placement and clipping,
  RedrawWindow in redraws, a window over the picture (hidden, drawn as
  before), uncovered, pause and resume, only 2 buffers mappable, and
  switched off from the menu. Mutations (no covering check, no hiding on
  pause) are caught.

## Reel 0.1.17 (2026-09-28): web addresses and yt-dlp's output

- **Web addresses** in !Reel and !ReelEGL: http://, https:// (AcornSSL,
  loaded by !Run) and HLS. **Open address…** on the icon bar menu: a
  window to type or paste one (Ctrl-V or Paste asks the clipboard's
  holder with Message_DataRequest; the text arrives through
  `<Wimp$Scrap>`, which is deleted after). Several pasted lines play at
  once. Or drop a file: a URI (&F91) or URL (&B28) file, a text file with
  one address a line (a playlist), or an M3U (#EXTINF titles used; an HLS
  playlist is played itself). Text dragged from another program
  (Message_DataSave) works too.
- **yt-dlp's output** (`player/sources.c`): `-g`'s address, or its two
  (best video and best sound apart, recognised by YouTube's mime=video /
  mime=audio), which play together; and `-j`/`-J`'s JSON: the title
  (UTF-8 to Latin-1), requested_formats' two addresses, the site's
  http_headers and user agent, a line (or `entries`) per video for a
  playlist. Where you stopped is remembered by the page's address
  (webpage_url). A web page's own address gets "that's a web page, not a
  video", pointing to yt-dlp in !Help.
- **reelcore reads addresses in a thread of its own**
  (`reelcore_open_source`, `ReelCoreSource`): it opens the address(es)
  and reads packets up to 10 s (or 32MB) ahead, so a slow connection
  never holds up the desktop; seeks go to the thread. With
  `REELCORE_ASYNC` the open returns at once and `reelcore_update()` says
  OPENING, then READY or FAILED: what was playing carries on meanwhile,
  and with nothing playing the window opens saying "Opening". FFmpeg's
  reconnect options, a 20 s timeout, and a whitelist of protocols (file,
  http, https, tcp, tls, crypto, data, httpproxy). Closing while opening
  returns at once. Two inputs (video and sound apart) work for files too.
- The time shows "Buffering" when the reading falls behind; Media info's
  Reading line gives what's read ahead, and Title, Address and Sound from
  rows. `reelcore_net()` for programs.
- Tests: `tests/host/httpserve.py` (a local server with Range requests,
  a header log and a never-answering mode); `net_test` (async open,
  playing, a seek giving the same picture as the file, video and sound
  from two addresses and from two files, headers, a missing address,
  closing while opening); `sources_test` (host gcc with the sanitizers:
  every kind of text above); reel_test's new phases: Open address with a
  Ctrl-V paste through the scrap file, playing while the last video
  carries on, yt-dlp -g's two addresses dropped with nothing playing, and
  a web page.

## 5.1.10-riscos11 (2026-09-28): https through AcornSSL

- **https** (and TLS generally: HLS over https, rtmps, tls:) through RISC
  OS's AcornSSL module, RISC OS 5.28 and later. FFmpeg patch 0018
  (`libavformat/tls_acornssl.c`, `--enable-riscos-acornssl`): the TCP
  connection is opened as usual and its RISC OS socket (UnixLib's
  `__get_ro_socket`) handed to `AcornSSL_CreateSession`; reads and writes
  go through `AcornSSL_Recv`/`AcornSSL_Send`, non-blocking, waiting on the
  socket with FFmpeg's timeout and interrupt callback. The handshake is
  driven with 1-byte peeks until AcornSSL stops answering ENOTCONN. The
  host name is set (SNI, and the certificate must be for that name);
  AcornSSL checks certificates against its own authorities and asks in the
  desktop about one it can't verify. `AcornSSL_Close` leaves the socket to
  the tcp: context. Written from AcornSSL's documentation and source
  (ROOL's RiscOS/Sources/Networking/Fetchers/AcornSSL, doc/AcornSSL, c/api).
- !Run loads AcornSSL (quiet if it isn't there); without it, https says
  what's missing.
- Tests: `tests/host/https.sh` runs the arm-linux ffmpeg/ffprobe with a
  pass-through stand-in for the module (`tests/host/fake_acornssl.c`)
  against a local HTTP server: the calls and their order for every
  session, the host name, a whole file read (and seeked) through it, a
  60-step handshake, a failed handshake, no module, and a server that
  never answers (-rw_timeout). The real handshake needs a Pi.
- !Help: https; the stale "ffplay has no desktop front end" limit removed.

## 5.1.10-riscos10 and Reel 0.1.16 (2026-09-28): UnixLib 5.0.1 and PThreadTicker

- **Relinked with UnixLib 5.0.1** (github.com/adyoull/riscos-unixlib,
  release v5.0.1): the thread timer's code runs from the new PThreadTicker
  module when it's loaded (else from a copy in the RMA, as before), and
  its Wimp filters are now registered even for threads started before
  Wimp_Initialise (SDL's), the cause of other tasks crashing while a
  threaded program ran (confirmed on a Pi with Warzone 2100). ffplay is
  the program here with several threads; the rest mostly run one.
- **PThreadTicker 0.01 is in each app** (!FFmpeg, !Reel, !ReelEGL, the EGL
  examples), from `third_party/pthreadticker` (BSD licence, in
  docs.Licences); each `!Run` (the examples' SetUp) loads a copy merged
  into !System first, else its own. Reel's log lists its version.
- `tools/check-unixlib.sh`: package.sh refuses a program not linked with
  UnixLib 5.0.1 (the 472-byte pthread block). `tools/check-versions.sh`
  also checks the module is the released one (sha256).
- Toolchain: the Warzone GCCSDK environment with UnixLib 5.0.1's
  `libunixlib.a`, `sched.h` and `unistd.h` (docs/SOURCES.md).

## Reel 0.1.15 (2026-09-28): resizing, and a better, lighter mini player

- **Resize grip** in the bottom right corner of the window and the mini
  player (the windows have no scroll bars, so the Wimp gave them no size
  icon): dragging it is the Wimp's own size drag. The mini player keeps
  the video's shape and remembers its width (`mini_width` in Choices).
- **Window size** on the window menu: Half, Actual size, Double (of the
  video's size in screen pixels) or Fit the screen; never narrower than
  the controls, kept on the screen.
- **The mini player looks better:** big reductions are halved first in
  reelcore (2x2 averages, NEON; `reelcore_halve_plane`), then swscale
  does the rest. At a quarter size (1280 -> 320) fast bilinear alone
  skipped three pixels in four (jagged, "blocky" detail): on the Simpsons
  trailer 23.7 dB from a Lanczos reference, against 35 dB for a box
  filter, which is what halving twice is. By instruction count it should
  cost about what it replaces (it reads the whole picture, but in NEON,
  and the colour conversion is then 1:1); the log's convert ms will show. Media info's Converting line says "halved twice,
  then swscale".
- **The mini player decodes a little less:** `REELCORE_FAST_LIGHT`, the
  deblocking filter skipped on pictures nothing is predicted from (most
  B-frames): about 14% less decoding on Big Buck Bunny 720p (x86, C),
  51.7 dB from normal decoding once shown at mini size, and nothing
  carries over to the normal window. `reelcore_set_fast` takes
  `REELCORE_FAST_OFF/ON/LIGHT` (0 and 1 as before).
- Tests: `halve_test` (NEON = C over 2,600 cases, whole pictures),
  options_test (light mode), reel_test (grip, Window size, the mini
  player's grip and saved width, light decoding in the mini player only).
  The host tests' reelcore is now built with NEON, as on RISC OS.

## 5.1.10-riscos9 and Reel 0.1.14 (2026-09-28): the standard Info window

- **Info on the icon bar menu is the usual RISC OS "About this program"
  window** in !FFmpeg, !Reel and !ReelEGL: a submenu showing Name,
  Purpose, Author (Andrew Youll) and Version with its date. Reel's Info
  used to open the media info window while a video was playing; that
  stays on the window's menu (and the I key). The window is built in code
  (`common/proginfo.h`, shared by all three).
- **One place for the version numbers:** `common/version.h`.
  `tools/check-versions.sh` (run by the host tests) checks that the
  Makefile, package.sh, build-ffmpeg.sh and the !Help headers agree with it.

## 5.1.10-riscos8 (2026-09-28): Convert saves next to the original

- **Convert works straight away.** The new file goes next to the
  original by default (holiday_720/mp4 beside holiday/mkv): the name
  field is filled with the full path, so clicking Convert is enough.
  Dragging the file icon to a directory display, or typing another full
  path, still puts it elsewhere. A bare name (no directory) is refused
  with a message saying what to do.
- **The file icon always shows.** It used the sprite `file_<type>` for
  the new file's type (MP4, via MimeMap); on a machine with no sprite for
  that type the icon was blank and there was nothing to drag. It now
  falls back to `file_xxx`, the Filer's "?" icon.
- Status line and interactive help say where the file will go.

## 5.1.10-riscos7 (2026-09-28): the Convert window

- **!FFmpeg converts files from the desktop.** Click the FFmpeg icon (or
  Convert... on its menu); dropping a file on the icon still plays it.
  One window, used from the top down (`frontend/convert.c`):
  - drop a file on it: ffprobe reads it and a line says what's in it
    (codecs, size, frame rate, interlaced or not, sound, length);
  - "Convert to": For playing here (MP4 up to 720 lines, easy to decode),
    MP4 same size, Smaller file (480 lines), Sound only MP3 / AAC, each
    with a one-line description; the options under it (format, size,
    quality, speed, sound, deinterlace, quick to decode, from/to) show
    what it chose, and changing one makes it Custom; what doesn't apply
    is greyed out;
  - save the RISC OS way: drag the file icon to a directory display, or
    type a full path and press Return / click Convert; the name is filled
    in from the source's (holiday_720/mp4) and its directory is kept;
  - a progress bar, "x real time" and the time left; Stop (the unfinished
    file is deleted); then Play, Show and, on failure, the reason and
    Log. An existing file is only replaced after asking; closing the
    window or quitting while converting asks first. Interactive help for
    every part.
  - ffprobe and ffmpeg run in task windows of their own (`*TaskWindow
    -task -txt`), their output coming back as messages: the desktop
    carries on. ffmpeg's `-progress pipe:1` drives the bar.
- **ffmpeg and ffplay get patches 0015–0017**: yadif (`-vf yadif`), HEVC
  chroma and swscale's scaling to RGB32 (ffplay's drawing) in NEON.
- The devkit has Reel 0.1.13's reelcore (speed, deinterlacing, sound
  tracks, picture modes) and the patched libraries.
- Tests: `convert_test` (34-step scripted desktop: probing, presets,
  menus, saving by drag, progress, Stop, replace, done, failure, help,
  quit), and the window's ffmpeg command lines run for real by ffmpeg
  5.1 on nine cases (presets, sizes, MKV with a part, silent, interlaced
  MPEG-2, HEVC, an odd size), the results checked with ffprobe.

## Reel 0.1.13: NEON for resizing and HEVC colour (2026-09-27)

- **FFmpeg patch 0017: swscale scaling to RGB32 in NEON.** With
  `SWS_FAST_BILINEAR` to RGBA/BGRA (what Reel uses whenever the picture
  isn't 1:1: full screen, resized windows, the mini player, Fill,
  Stretch; and ffplay's direct drawing) the 32-bit ARM path was C.
  Now the fast bilinear horizontal scalers (8 outputs a step, source
  bytes through `vtbl`, down to about 1.85x) and the `yuv2rgbx32`
  output (`_1` and `_2`) are NEON. The 32 bpp colour tables can't be
  looked up in NEON, so `ff_yuv2rgb_c_init_tables` now also stores them
  as arithmetic (`SwsContext.yuv2rgb_arith`), which gives exactly the
  tables' values.
- **FFmpeg patch 0016: HEVC chroma motion compensation in NEON**
  (`put_hevc_epel`, `_uni`, `_bi`; h, v, hv; widths 2–64). FFmpeg's
  32-bit ARM NEON had luma only. The whole-sample chroma copies now use
  luma's NEON too.
- **Tests:** `hevc_epel_test` (1,900 cases against the C; four
  deliberate bugs all caught) and HEVC decodes giving the same pictures
  with NEON, with `-cpuflags 0` and on x86 FFmpeg; `swscale_rgb_test`
  (2,747 cases: the functions alone over many scales and colour
  settings, and whole pictures in the same context with and without the
  NEON functions). FFmpeg's checkasm: all 962 pass. qemu emulates NEON
  slowly, so the speed-up is for the Pi to show.
- **The log** now has, once a second, what each picture cost: decoding,
  converting (and to what size), drawing and deinterlacing, so full
  screen can be measured.
- The ffmpeg/ffplay programs get patches 0015–0017 in the next FFmpeg
  package; Reel's zip carries them in `docs.source.patches`.

## Reel 0.1.12: deinterlacing, with yadif in NEON (2026-09-27)

- **Deinterlace** (window menu: Auto, On, Off; D; remembered). reelcore
  runs FFmpeg's yadif (buffer → yadif=send_frame → buffersink, one
  picture a frame, no threads) on the pictures as they're decoded, so
  redraws, resizes and the mini player cost nothing extra. Auto makes
  the filter only at the first picture marked interlaced: progressive
  video goes straight through as before. Seeking drops what yadif held;
  at the end its last picture is flushed.
- **FFmpeg patch 0015: yadif's line filter in NEON for 32-bit ARM**
  (`libavfilter/arm/vf_yadif_neon.S`). FFmpeg 5.1 has x86 versions only,
  so ARM ran the C. Eight pixels an iteration, 16-bit sums, byte-element
  loads and stores (no alignment needed). Bit-exact: `yadif_test`
  compares it with the C on 56,064 lines (every mode, parity, edge line,
  width 1–70 and wide, every alignment; four deliberate bugs are all
  caught), and `ffmpeg -vf yadif` gives the same pictures with NEON,
  with `-cpuflags 0` and on x86 FFmpeg. The ffmpeg/ffplay programs get it
  in the next FFmpeg package; Reel has it now (its zip carries the patch
  in `docs.source.patches`).
- Media info: **Scan** (progressive / interlaced, which field first) and a
  **Deinterlacing** stats row (mode, ms a picture, pictures interlaced).
- reelcore: `reelcore_set_deinterlace`, `reelcore_deinterlace`,
  `REELCORE_DEINT_*`; stats `deinterlace`, `interlaced`, `deinterlaced`,
  `deinterlace_time`. Its filter graphs (atempo too) are made with one
  thread.
- Tests: `yadif_test`, the whole-picture yadif comparison, `deint_test`
  (Auto matches x86 FFmpeg's yadif at the same frame, Off matches the
  plain decode, seeking, progressive left alone), and Reel's menu and D
  key in `reel_test`.

## Reel 0.1.11: playback options and a mini player (2026-09-27)

- **Volume:** a green bar in the controls row; click along it. It's
  remembered (`Choices:Reel.Choices`). The bar is roughly how loud it
  sounds (the level given to SharedSoundBuffer is its square).
- **Mini player** (M, or the window menu): a small window with no title
  bar, bottom right just above the icon bar (it asks the Wimp where the
  icon bar is): the picture, Play/Pause, the position bar and Normal.
  Drag the picture to move it (remembered); double-click it, click
  Normal or press M for the normal window again. **Keep on top** (window
  menu, remembered): RISC OS has no always-on-top for ordinary windows,
  so while playing the mini player looks once a second and, if another
  window has been opened over it, comes back to the front (never taking
  the caret).
- **Vsync (full screen)**, on by default: full screen waits for the
  screen's refresh before each picture (Reel: OS_Byte 19; ReelEGL:
  eglSwapInterval 1 with a back buffer). Untick it for Direct (ReelEGL:
  `EGL_SINGLE_BUFFER`, straight into screen memory). `<App>$NoVsync`
  starts with it off (was ReelEGL's `Direct`, `ReelEGL$NoDirect`).
- **Speed** 0.5x to 2x (window menu); the sound keeps its pitch. The time
  shows e.g. "1.5x".
- **Picture:** Fit (as before), Fill (crop), Original size, Stretch.
- **Fast decode:** skips the deblocking filter, for files that are too
  much for the machine.
- **Sound track:** choose among a file's sound tracks.
- **Playlist:** files dropped together (one drag) play in turn; Shift
  adds to the list. N/P, and the Playlist submenu (with Clear the rest);
  the title shows "(2/5)". Loop loops the list.
- **Carry on:** Reel remembers where you stopped each file (the last 100,
  `Choices:Reel.Resume`) and asks whether to carry on from there.
- **A-B repeat** (A, or the window menu): set A, set B, off. The time
  shows " A" / " AB".
- The window menu is regrouped, with submenus; the time field is wider
  (hour-long files and the new marks fit); the narrowest window is 1096
  OS units.
- **reelcore:** `reelcore_set_speed` (FFmpeg's atempo; programs now link
  `-lavfilter -lpostproc`), `reelcore_set_fast`, `reelcore_volume`,
  `reelcore_audio_tracks`, `_track`, `_track_name`, `set_audio_track`;
  `REELCORE_FILL` and `REELCORE_ORIGINAL` for `reelcore_draw_pixels` and
  `ffegl_draw_surface`. The riscos6 devkit has 0.1.10's reelcore; these
  reach the devkit with the next FFmpeg release.
- Tests: `options_test` (speed, fast decoding, sound tracks, picture
  modes); `reel_test` drives all of the above through the fake Wimp in
  the Reel, SharedSoundBuffer and ReelEGL builds.

## Reel 0.1.10: reelcore and ffegl split (2026-09-27)

- **ffegl was a player core with EGL attached; now they are two
  libraries.** The name said "FFmpeg into EGL", but most of it (reading,
  decoding, the sound and clock, frame skipping, stats) had nothing to do
  with EGL, and !Reel used it with no EGL at all.
  - **reelcore** (`reelcore/`): the player core, no EGL. Everything that
    was `ffegl_*` except the two EGL calls is now `reelcore_*`
    (`reelcore_open`, `reelcore_update`, `reelcore_draw_pixels`,
    `reelcore_idle_time`, `reelcore_stats`, ...), `FFEGLVideo` is
    `ReelCore`, the flags and results are `REELCORE_*`, and
    `FFEGL_AUDIO` is `REELCORE_AUDIO`. New: `reelcore_frame_size()`, and
    `reelcore_attach()`/`reelcore_attachment()` for a layer's own state.
  - **ffegl** (`ffegl/`): only the EGL side, on top of reelcore:
    `ffegl_draw_surface()` and `ffegl_texture()` (its EGLImage state is
    kept with the video through `reelcore_attach` and freed by
    `reelcore_close`). Built `-DFFEGL_NO_TEXTURE` it has no OpenGL.
  - !Reel links reelcore only; !ReelEGL, videowin and videocube link
    reelcore and ffegl. The devkit has `libreelcore.a` + `reelcore.h` as
    well as `libffegl.a` + `ffegl.h`. `build/build-ffegl.sh` is now
    `build/build-apps.sh` (`make apps`; `make ffegl` still works).
- No change in what the players do. All host tests pass, including
  ffegl_texture against riscos-mesa's real EGL and OSMesa.

## 5.1.10-riscos6 (2026-09-27): first GitHub release

The same `ffmpeg`, `ffprobe` and `ffplay` as riscos5; what changed is
around them:

- **!FFmpeg:** `!Run`, `Task` and the EGL examples' `SetUp` load
  SharedSound, StreamManager and SharedSoundBuffer (they were only merged
  into `!System`, never loaded, so ffplay had no working sound device).
  The icon's menu has **Log** (ffplay's messages, now at verbose level,
  in `<Wimp$ScrapDir>.ffplay/log`).
- **The player library** (ffegl then; reelcore and ffegl from Reel
  0.1.10), in the devkit and the EGL examples, as in Reel 0.1.5–0.1.10: sound straight to SharedSoundBuffer
  on RISC OS (no SDL audio thread); the file read for the sound
  separately from video decoding (the trailer freeze); frame skipping
  when behind; `ffegl_debug`, `ffegl_set_log`, `ffegl_media_info`,
  `ffegl_stats` and `ffegl_idle_time`.
- `COPYING` (GPL version 2) added.

## Reel 0.1.9 (2026-09-27)

- **Sleeps between pictures.** On the Pi, Media info showed ReelEGL
  decoding the 1280x544 trailer in 23% of the time (9.6 ms a picture,
  4.3x real time), converting and drawing in under 5 ms, yet Task
  Manager showed 56%: the rest was 5200 null events a second asking
  whether the next picture was due yet.
  - ffegl: `ffegl_idle_time()` says how long the caller may sleep after
    `ffegl_update()`: 0 while there's work (fewer than 3 pictures
    decoded, sound under 0.35 s queued, a picture due), otherwise the
    time to the next picture, at most 0.1 s.
  - Reel polls with `Wimp_PollIdle` until then (whole centiseconds, so it
    wakes on or just before time; the last few ms are polled as before).
    The next pictures are decoded before sleeping, so showing one on
    time needs only the wake-up.
  - Media info's Desktop line and the log show how much of the time Reel
    was asleep. `Reel$NoSleep` / `ReelEGL$NoSleep` polls flat out.
- Test: `idle_test` plays the same clip polling every 0.5 ms and
  sleeping as Reel does (waking up to 1 cs late at random, 8 ms a
  decode): the same 150 pictures, none late, worst 4.7 ms after its time
  (flat out: up to 5 ms early), with 607 wake-ups instead of 9633.
  reel_test checks Wimp_PollIdle is used.

## Reel 0.1.8 (2026-09-27)

- **Media info window** (window menu "Media info", or I), replacing the
  File info error box, which stopped the picture while it was open. It
  stays open while playing and follows the file being played.
  - At the top, "stats for nerds", every second while playing (and on
    pause): position and clock (sound or timer) and the gap between
    them; pictures shown a second against the video's rate, late ones;
    pictures decoded a second, ms each and times real time; decoding
    load (pictures and sound); frame skipping; swscale time a picture
    and its size; drawing time a picture (OS_SpriteOp, or EGL); pictures
    and packets waiting; the sound output and queue; the reading rate;
    null events a second and the screen mode.
  - Below, from `ffegl_media_info()`: file (name, title, container,
    length, size, bit rate, streams), video (codec, profile, level,
    size, aspect, frame rate, pixel format, colours, bit rate, frames,
    decoder, B-frame reordering), audio (codec, profile e.g. HE-AAC,
    channels and layout, sample rate and format, bit rate, language,
    decoder) and the sound output.
  - Drawn with Wimp_TextOp in the desktop font; the stats are redrawn
    with Wimp_UpdateWindow (no flicker from ForceRedraw).
- ffegl: `ffegl_media_info()` and `ffegl_stats()` (running totals:
  pictures decoded and shown, decoding, sound and swscale time, sizes,
  queues, sound counts, bytes read).
- Test: reel_test opens the window with I, checks its text (codec,
  sample rate, the stats after a second of playing) and closes it.

## Reel 0.1.7 (2026-09-27)

- **The trailer froze after half a second, now with SharedSoundBuffer.**
  Reel 0.1.6's log on the Pi: the sound started (SharedSoundBuffer 0.07,
  StreamManager 0.03 and SharedSound 1.20 all loaded), played 0.51 s,
  then `queued 0.00 s ... 7 pictures waiting` for good: the clock stayed
  at 0.49 s.
  - In this file the video is read about a second ahead of its sound.
    ffegl read packets only while fewer than 7 pictures were waiting, so
    with 7 decoded pictures (all ahead of the clock) it stopped reading;
    the sound ran dry; the clock (the sound) stopped; no picture became
    due. Every build since ffegl 1 had this; the SDL stall fallback hid it
    in 0.1.3.
  - Now ffegl keeps the video packets it reads (compressed) in a queue
    and decodes them separately: the file is read as far as the sound
    needs (0.5 s queued), however far the video is ahead or however slow
    it decodes (up to 48 MB of packets).
- **Falling behind:** 0.3 s behind the clock, ffegl skips decoding
  non-reference frames (`skip_frame = AVDISCARD_NONREF`, mostly
  B-frames); 1.5 s behind, keyframes only; back to every frame once
  within 0.05 s. The log says when ("0.32 s behind: skipping
  non-reference frames"), and `ffegl_debug()` shows the packets waiting
  and the skip level.
- Tests: `chunky_h264_aac_322_184.mp4` (fragments of 1.5 s, video then
  sound) freezes 0.1.6 and plays with 0.1.7 in `reel_ssb_test`;
  `slow_test` makes decoding cost more than real time and checks that
  the pictures keep up with the sound.

## Reel 0.1.6, !FFmpeg front end log (2026-09-27)

- **Reel and ReelEGL write a log**: `<Wimp$ScrapDir>.ReelLog`
  (`ReelEGLLog`), started afresh at each start and flushed line by line.
  - Start: the build, the versions of SharedSound, StreamManager,
    SharedSoundBuffer, SharedUnixLibrary and VFPSupport (from their help
    strings, or "not loaded"), `FFEGL_AUDIO`, `SDL_AUDIODRIVER`, the
    screen mode; ReelEGL adds EGL's version and each surface made.
  - Each file opened (`ffegl_info`), the window size, pause, seek, full
    screen, mode changes, the end, close, quit, and every error box.
  - FFmpeg's messages at verbose level, with repeats folded.
  - Once a second while playing: `ffegl_debug()` (position, clock and
    which one, pictures waiting, late frames; the sound output, queued
    time, StreamManager's added/played counts, bytes waiting, refused
    blocks), plus null events and pictures shown.
  - `Reel$Log` / `ReelEGL$Log` names another file, or `off`. "Log" on
    the icon bar menu opens it (Filer_Run).
- ffegl: new `ffegl_debug()` and `ffegl_set_log()`; SharedSoundBuffer
  events (stream opened, sound starts, first refused block, reopen
  failures) and SDL's driver name are logged at verbose level; the
  stall warning includes the `ffegl_debug()` line.
- !FFmpeg: ffplay runs with `-loglevel verbose` into
  `<Wimp$ScrapDir>.ffplay/log`, and the icon's menu has "Log" to open it.
- Tests: `reel_ssb_test` and `reelegl_test` check the log's contents.

## Reel 0.1.5 (2026-09-27)

- **Still no sound on the Pi with the modules loaded.** With SharedSound,
  StreamManager and SharedSoundBuffer loaded (`*Help SharedSoundBuffer`
  shows 0.07), ReelEGL still said "the sound device isn't playing": SDL's
  RISC OS driver opened, but the sound queued with `SDL_QueueAudio` was
  never taken. SDL takes it on its own audio thread, and in a Wimp task
  UnixLib's threads only run while the task is paged in; Reel spends most
  of its time in `Wimp_Poll` or decoding.
- ffegl now gives the sound to SharedSoundBuffer/StreamManager itself on
  RISC OS, from `ffegl_update()`, in 2048-frame blocks (0.5 s kept
  queued). StreamManager plays it from interrupts, so no thread is
  involved. The clock is `StreamManager_BufferStats` (added - played).
  Seeking closes and reopens the stream. `FFEGL_AUDIO=sdl` goes back to
  SDL. This covers Reel, ReelEGL, videowin and videocube.
- Test: `reel_ssb_test` runs the Reel script with ffegl built
  `-DFFEGL_SSB` against fake SharedSoundBuffer SWIs.

## Reel 0.1.4, and !FFmpeg 5.1.10-riscos5 repackaged (2026-09-27)

- **No sound on the Pi:** SharedSoundBuffer, StreamManager and SharedSound
  were merged into `!System` but never loaded (`*Help SharedSoundBuffer`
  gave "No help found"). Nothing had asked for them.
  - SDL then fell back to UnixLib's `/dev/dsp`, which accepted the sound
    and didn't play it. Reel showed "the sound device isn't playing", and
    ffplay played silently.
  - The `!Run` files of `!FFmpeg`, `!Reel` and `!ReelEGL`, `!FFmpeg.Task`
    and the EGL examples' `SetUp` now RMEnsure/RMLoad `SSound`,
    `StreamMan` and `SSBuffer` from `System:Modules`, the same lines as
    Warzone 2100's `!Run`.
- ffegl (so Reel, ReelEGL, videowin and videocube) now asks SDL for its
  RISC OS sound driver only (`SDL_AUDIODRIVER=riscos` unless it's already
  set). If the modules are missing there's no sound device, and
  `ffegl_info()` gives SDL's reason, e.g. "no sound device: RISC OS audio:
  SharedSoundBuffer/StreamManager not loaded".
- The `!FFmpeg` zip was repackaged with the new `!Run`, `Task` and
  `!Help`. The programs in it are unchanged.

## Reel 0.1.3 (2026-09-27)

- **A sound device that doesn't play no longer freezes the picture.** On
  the Pi, a trailer with sound showed its first frame and stopped. Reel
  follows the sound clock, and that clock doesn't move while the device
  takes none of the queued sound.
  - ffegl now watches for this: if nothing queued is played for 1 s while
    playing, it stops sending sound, pauses the device and carries on
    with its timer.
  - The stall timer restarts after a seek and after a pause.
  - `ffegl_has_audio()` returns 0 then, and `ffegl_info()` says "the
    sound device isn't playing".
  - Tested with a fake device that opens but never plays: all 25 frames
    are shown, finishing after about 2 s for a 1 s clip.
- HE-AAC (the trailer's sound) decodes under the alignment-trapping QEMU,
  and so does its H.264 Main 1280x544 picture. The file itself was never
  the problem.

## Reel 0.1.2 (2026-09-27)

- **!ReelEGL:** the same player (`player/reel.c` built with `-DREEL_EGL`),
  drawing through riscos-mesa's EGL. It's a working example of
  `ffegl_draw_surface` in a real program.
  - In the window, the picture is a work-area EGL surface (fixed size, top
    of the work area) above the ordinary Wimp icons. Frames arrive by
    `EGL_KHR_lock_surface` and `eglSwapBuffers`; redraws go through
    `eglPlotSurfaceRISCOS` in the task's own redraw loop.
  - Full screen uses the whole-screen surface. It's direct into screen
    memory by default (`EGL_SINGLE_BUFFER`, "Direct" on the window menu,
    `ReelEGL$NoDirect`), otherwise a sprite plotted after vsync.
  - It links riscos-mesa's EGL and OSMesa: 28.8 MB text.
  - It passes the same scripted test as Reel, against a fake EGL: the
    frames shown, work-area surface sizes, direct full screen, no surfaces
    left over.
- Reel: the picture is refreshed only when the window is actually resized
  (not on every move), and the drawing sits behind a small internal layer
  shared by both builds.
- ffegl: `FFEGL_NO_TEXTURE` (EGL surfaces without the GL texture code).
- Reel 0.1.1's File info said "no sound" for Big Buck Bunny 720p 10s 30MB.
  That's correct: the file has no sound track.

## Reel 0.1.1 (2026-09-27)

- **!Reel, a video player for the desktop** (`player/reel.c`,
  `dist/Reel-0.1.1.zip`). It's a native Wimp app built on ffegl: an icon
  bar icon, and a window with the picture above a row of controls.
  - Controls: Play/Pause, back and forward 10 s, a position bar you can
    click, the time, and Full. Keys: Space, the arrow keys, F, Escape
    and Q.
  - Open a video by dropping it on the icon or the window. Double-clicking
    in the Filer also works while Reel is loaded, for filetypes MimeMap
    calls video/*.
  - The picture is scaled into the window by swscale (NEON), letterboxed,
    and plotted as a 32bpp sprite in the screen's colour order. The window
    can be resized.
  - Full screen is a borderless window over the whole screen.
  - Sound goes through SDL2 (SharedSoundBuffer) and is the clock. There
    are no threads of its own: decoding happens on null events.
  - The window menu has File info (codec, size, fps, sound, frames
    skipped), Full screen, Loop and Close. The icon menu has Info, Loop
    and Quit.
  - It uses no EGL or OpenGL (ffegl built with `FFEGL_NO_GL`), so it's a
    20 MB program.
  - **Test:** `tests/host/reel_test.c` drives the whole app through a
    scripted fake desktop, with real decoding under the alignment-trapping
    QEMU. It checks playing (the plotted pixels are the frame),
    pause/resume, the position bar, full screen and back, resize, a second
    file, a directory refused, and double-click claiming only video types.
- ffegl: `ffegl_info()` (a description of the streams),
  `ffegl_dropped_frames()`, and the `FFEGL_NO_GL` build option.

## 5.1.10-riscos5 (2026-09-27)

- **An icon bar icon:** double-clicking `!FFmpeg` now puts the FFmpeg icon
  on the icon bar, as well as setting up the commands.
  - Drop a video or sound file on the icon and it plays in ffplay. Each
    file gets its own window and task, and messages go to
    `<Wimp$ScrapDir>.ffplay/log`.
  - The icon's menu has Info, Full screen (plays with `-fs`) and Quit.
  - It's a small C program (`frontend/fffront.c`, `!RunImage`, 512K); only
    one copy runs. It has a host test with a scripted fake Wimp.
  - Repackaged the same day: the first version said "File name '.Task' not
    recognised" when `!FFmpeg` was started from the boot sequence
    (Configure > Boot > Look at). It used `FFmpeg$Dir` when a file was
    dropped, and that can hold a reference like `<Obey$Dir>` which by then
    points elsewhere. It now works out its full directory when it starts
    (`OS_FSControl` 37, else its own command line).

## 5.1.10-riscos4 (2026-09-27)

- **The real cause of the Pi crash** ("abort on data transfer", which
  `*Where` placed in VFPSupport; an EMT trap in `hl_motion` in the log):
  - FFmpeg's `ff_put_h264_chroma_mc2_neon` copied 2x2 chroma pixels with
    `ldrh` from a source that can be at an odd address. That happens in
    H.264 4x4 sub-partitions at whole-pixel chroma positions.
  - Big Buck Bunny 720p hits it within the first frames; the old test clips
    never used 4x4 partitions.
  - Byte loads now (patch 0014). Single-threaded ffmpeg crashed the same
    way, so it was never a thread or TaskWindow problem.
- **New "motion" test clips** (`tests/qemu/samples/mv_*`). They scroll and
  zoom, and are encoded with every partition size, B-frames, weighted
  prediction, MBAFF, CAVLC, 10-bit and 4:2:2 (H.264), plus HEVC, VP8, VP9,
  AV1, MPEG-4 qpel/4MV, interlaced MPEG-2, Theora, WMV2 and H.263.
  - All decode under the alignment-trapping QEMU with NEON output equal
    to C.
  - The fixed mc2 was compared with an x86 ffmpeg frame by frame.
- riscos3's change (switching the VFP context off around `Wimp_StartTask`)
  stays: UnixLib does the same, and it's correct. But it wasn't this
  crash.

## 5.1.10-riscos3 (2026-09-27)

- **ffplay from a TaskWindow crashed on the Pi** ("Internal error: abort on
  data transfer" in VFPSupport; the new ffplay's log showed an EMT trap in
  its video decoder thread). *Note (riscos4): the explanation below was
  wrong; the crash was the unaligned `ldrh` fixed in riscos4.* The ffplay in the TaskWindow started the new
  task with its own VFP (floating point) context still active. VFPSupport
  later saved that context to its old stack address, which by then
  belonged to the new ffplay, and corrupted it. ffplay now switches its
  context off around `Wimp_StartTask`, as UnixLib does before running
  another program (patch 0013).
- **Built with the fixed UnixLib from the Warzone 2100 port's toolchain**
  (caea90c3): the pthread ticker's code is in the RMA, so a thread switch
  can't jump into ffplay's memory while another task is paged in. ffplay
  runs several threads (reading, decoding, SDL's timer). Warzone 2100 hit
  exactly that abort.

## 5.1.10-riscos2 (2026-09-27)

- **`ffmpeg -f egl`:** a new output device that shows video through
  riscos-mesa's EGL, in a desktop window or the whole screen (optionally
  straight into screen memory). swscale converts and scales each frame
  into the surface in one pass.
- **ffegl:** a library for EGL and OpenGL programs. It plays a file with
  sound (SDL2 audio is the clock) into an EGL surface, an OpenGL texture
  or any 32bpp memory, and has pause, seek and loop. It is in the devkit
  (`ffegl.h`, `libffegl.a`), with two examples in
  FFmpeg-EGL-examples: `videowin` and `videocube`.
- **Textures without a copy:** with riscos-mesa 20.3.5-7pre12 or later,
  `ffegl_texture()` binds a sprite to the texture through an EGLImage
  (`EGL_KHR_image_pixmap`, `GL_OES_EGL_image`), and swscale writes each
  frame straight into it. Older riscos-mesa, or `FFEGL_NO_EGLIMAGE=1`,
  copies with `glTexSubImage2D`.
- **ffplay** converts and scales frames straight into its window surface,
  in the screen's format, instead of through an SDL texture and a second
  scale. `FFPLAY_SCALE` picks the scaler; `FFPLAY_RENDERER=sdl` gives the
  old path.
- **Fixes found by the new tests** (both also affect ffplay):
  - swscale's NEON YUV→RGBA store demanded 16-byte aligned
    destinations and would fault on RISC OS otherwise (patch 0009);
  - swscale's NEON converters reported 0 lines done, which callers took as
    failure (patch 0010).
- **ffplay in a TaskWindow:** riscos1 stopped with "Failed to create
  window or renderer: Wimp_Initialise failed: Window Manager is currently
  in use", because a TaskWindow can't open windows. ffplay now starts
  itself as a desktop task of its own (through the new `Task` Obey file,
  which sets the WimpSlot), with its messages in
  `<Wimp$ScrapDir>.ffplay/log`; `FFPLAY_NEWTASK=0` turns this off
  (patch 0013). `-f egl` gives a clear error there instead of taking over
  the screen (patch 0012).
- `ffmpeg -version` now says `5.1.10-riscos2` rather than a git hash.
- The programs are larger (about 31MB) because they include riscos-mesa's
  EGL and OpenGL; the WimpSlot is now 44MB.
- Built with the riscos-mesa 20.3.5-7pre12 devkit.

## 5.1.10-riscos1 (2026-09-27)

First release: FFmpeg 5.1.10 LTS for RISC OS on ARMv7 + NEON.

- Built with GCCSDK GCC 10.2 (`-O3`, `-mfpu=neon-vfpv3`,
  `-mtune=cortex-a72`, `-fstack-clash-protection`) as static AIF programs:
  `ffmpeg`, `ffprobe`, `ffplay`.
- With x264, dav1d 1.5.4, LAME 3.100, Opus 1.5.2 and Vorbis 1.3.7.
  `ffplay` uses riscos-mesa's SDL2 overlay without GL.
- **Alignment:** the NEON assembly of FFmpeg, dav1d and x264 is rewritten
  at build time so it can't fault with RISC OS's alignment checking on.
  The ARMv6 media routines are not used, and a few core-ARM loads and
  stores are patched (docs/ALIGNMENT.md). It was checked under a QEMU that
  traps like RISC OS: all three checkasm suites and 110 decode/encode jobs
  pass.
- **RISC OS file names** work for input and output (`SDFS::…`,
  `<Var$Dir>.…`), and a `/ext` suffix picks the output format.
- **Memory:** the heap is in the "FFmpeg Heap" dynamic area (512 MB
  maximum).
- **ffplay** converts frames to the screen's own 32bpp format with
  swscale's NEON code.
- **Bench:** an Obey file that compares NEON with plain C on your machine.
- Not tested on RISC OS hardware yet.
