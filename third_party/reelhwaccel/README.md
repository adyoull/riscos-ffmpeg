# riscos-reelhwaccel devkit

What FFmpeg and Reel need to decode H.264 on the Raspberry Pi's VideoCore,
and HEVC on the Pi 4's HEVC block, from RISC OS:

| File | What it is |
|---|---|
| `include/vcdec.h`, `lib/libvcdec.a` | vcdec: the VideoCore's H.264 decoder through RISC OS's VCHIQ module and the firmware's MMAL service. GCCSDK GCC 10, ARMv7, hard float. |
| `ffmpeg/0001-avcodec-h264_vchiq.patch` | FFmpeg 5.1.10's `h264_vchiq` decoder on top of vcdec (`git am`, or `patch -p1`). It applies to plain 5.1.10 and on top of riscos-ffmpeg's series. |
| `include/hwhevcdec.h`, `include/hevc_ctrls.h`, `lib/libhevcdec.a` | hevcdec: the Pi 4's HEVC block (rpivid), driven directly from user mode, fed V4L2 stateless HEVC controls. GCCSDK GCC 10, ARMv7, hard float, NEON. |
| `ffmpeg/0002-avcodec-hevc_hwdec.patch` | FFmpeg 5.1.10's `hevc_hwdec` hwaccel and decoder on top of hevcdec. Apply after 0001; it applies on top of riscos-ffmpeg's series too. |

## FFmpeg

Apply the patch, then configure with:

    --enable-gpl --enable-vchiq --extra-cflags=-I<devkit>/include --extra-ldflags=-L<devkit>/lib

`h264_vchiq` is not FFmpeg's default H.264 decoder; ask for it by name
(`-c:v h264_vchiq`, or `avcodec_find_decoder_by_name("h264_vchiq")`). It
gives ordinary YUV420P frames, in display order with their pts.

- **Refused at open with AVERROR(ENOSYS):** streams the VideoCore can't take, so the caller can open `h264` instead. That covers Baseline/Main/High only; 8-bit 4:2:0 only; at most 1920x1088; 1080p only with gpu_mem of 128 MB or more; and machines without VCHIQ.
- **A failure part way** gives AVERROR_EXTERNAL. After that the decoder takes packets until the end of the stream, then gives EOF.
- **Waiting:** `avcodec_receive_frame` waits for the VideoCore only while its input is full (about one picture's time) or at the end of the stream. Otherwise it gives EAGAIN.
- **Seeking:** `avcodec_flush_buffers` is a seek. After the end of the stream, vcdec creates the decoder again, so nothing is lost.
- **Options:** `sync_receive` (wait for each picture's transfer as it arrives), `gpu_mem_check` (default on), `pci_memory` (default off: see below), `zero_copy` (default on) and `out_buffers` (0: 6 with zero_copy, else 3; up to 16).
- **Zero-copy (devkit 0.2.1, vcdec 0.4.1):** a frame's planes are vcdec's own picture buffer, held until the frame is freed. Frames are read only (`AV_BUFFER_FLAG_READONLY`: `av_frame_make_writable` copies). The decoder has `out_buffers` buffers and keeps 2 for itself, so with the default 6 a caller can keep up to 4 frames at once without copies; beyond that (or from PCI memory) frames are ordinary copies, so the decoder never runs dry. Frames may outlive the decoder: its memory goes with the last of them. Free frames in the thread that decodes.
- **Memory (vcdec 0.4):** pictures arrive in a cacheable Physical Memory Pool and are copied out by LDM 8: 1080p at 82 pictures a second on a Pi 4, against 64 from PCI memory. Without a pool (RISC OS before 5.23, or no contiguous pages) vcdec uses PCI memory and logs why (`-v verbose`). `-pci_memory 1` asks for PCI memory, as an escape hatch.
- **The program's page at &8000 (vcdec 0.4.2, hevcdec 0.1.6):** the pools' contiguous pages never include it. RISC OS may hand over a running program's pages for contiguous memory, copying the program elsewhere, and ARMEABISupport (which recognises a program by that page's physical address) then loses it: stacks left behind, and later an EMT trap ("code 6") in another program. `app_page_moves` in either library's stats counts claims after which the page moved anyway (should be 0). On a RISC OS before 5.29 (which can't be asked to avoid a range) a pool that would include it is refused: vcdec uses PCI memory instead, and hevcdec reports that it has no contiguous memory, saying why (the case the libraries were already built to handle).
- **Linking:** add `-lvcdec` after `-lavcodec` when linking programs.

## HEVC: hevc_hwdec

Apply 0002 after 0001, then add to the configure line above:

    --enable-libhevcdec --enable-decoder=hevc_hwdec --enable-hwaccel=hevc_hwdec

(`--enable-libhevcdec` needs `--enable-gpl`; it also selects the `hevc`
decoder, which `hevc_hwdec` runs inside.) Link programs with `-lhevcdec`
after `-lavcodec`.

`hevc_hwdec` is FFmpeg's own HEVC decoder with the block doing the
decoding: FFmpeg parses the stream and keeps the reference pictures, the
block decodes each picture, and the decoder gives ordinary YUV420P frames
(YUV420P10 for 10-bit streams, devkit 0.2.5)
(converted from the block's 128-byte column format), with their pts and
cropping applied. Ask for it by name (`-c:v hevc_hwdec`, or
`avcodec_find_decoder_by_name("hevc_hwdec")`).

- **Refused with AVERROR(ENOSYS),** so the caller can open `hevc` instead: anything but 8-bit or 10-bit 4:2:0 (4:2:2, 4:4:4, 12-bit); over 4096x4096; a picture size that grows, or a depth that changes, part way; machines without the block (not a Pi 4).
- **10-bit (devkit 0.2.5, hevcdec 0.1.7):** YUV420P10 frames, exactly `hevc`'s. With `-output_8bit 1` they come out as YUV420P instead, each sample the 10-bit one's top 8 bits (truncated, no dithering): for a player that only shows 8-bit, one conversion instead of two.
- **Keeping the block busy (devkit 0.2.6, hevcdec 0.1.8):** while hevc_hwdec converts a picture, hevcdec starts the block's next phase as soon as the last one finishes, so the next picture is decoded during the copy: each picture now costs the longer of the two, not their sum.
- **4K:** up to 4096x4096. A 3840x2160 frame is 12 MB of contiguous memory (16 MB at 10-bit), and the decoder makes as many as the stream's DPB needs plus those the caller holds. Streams with their parameter sets in the extradata (MP4, MKV) are refused at open; raw streams at their first picture.
- **A picture the block fails on** comes out with `decode_error_flags` set (FFmpeg logs "corrupt decoded frame"); decoding carries on.
- **drop_before (devkit 0.2.7):** as riscos-ffmpeg's h264_vchiq option: a pts in pkt_timebase (default INT64_MIN, off). Pictures with an earlier pts are still decoded by the block (later pictures may refer to them) but are neither converted nor given out. A player that has fallen behind sets it as it goes (two frames behind its clock, or the seek point while seeking), instead of skip_frame, which would keep pictures from the block itself.
- **output_hw (devkit 0.2.8, hevcdec 0.1.9): one copy instead of two.** With `output_hw=1`, `avcodec_receive_frame` gives the pictures unconverted, as `AV_PIX_FMT_HEVCDEC` frames:
  - `data[3]` is the `hevcdec_frame *`;
  - the window is from (`crop_left`, `crop_top`) to (`width`, `height`); FFmpeg has already taken the right and bottom crops off width and height.
  - **Converting at show time:** convert a frame when it is shown, straight into your own buffer, with `hevcdec_frame_to_i420(hevcdec_frame_decoder(f), f, ...)`. For a 10-bit stream use `hevcdec_frame_to_i420_16` for 16-bit samples, or `_i420` for 8-bit. For a 4K picture into an HD-sized overlay use `hevcdec_frame_to_i420_half`: 2x2 means, x a multiple of 4.
  - A conversion waits for the picture if the block is still decoding it.
  - **Lifetime:** each frame holds one of hevcdec's frames (12 MB at 4K) until freed, on the thread that decodes. hevcdec stays open until the decoder is closed and the last frame is freed. While it is open, no other hevcdec can be opened.
  - **Failures:** a picture the block failed is flagged in `decode_error_flags` only if that was known when it was given out.
- **Fixes from a code audit (devkit 0.2.9, hevcdec 0.1.10):**
  - With `output_hw`, frames kept past `avcodec_free_context` no longer make hevcdec log through the freed context when the last one is freed (a use-after-free).
  - A damaged stream marking more than 16 reference pictures is refused (AVERROR_INVALIDDATA for that picture) instead of overrunning the 16-entry DPB list; hevcdec itself refuses pictures with more than 16 references, references outside the DPB, or slices longer than their data.
  - A picture whose slices FFmpeg gave up on part way can no longer be sent with another picture's slices; a frame never given to the block comes out flagged (`decode_error_flags`) rather than as a good picture.
  - A stream whose picture size changes part way (smaller, then back) restarts hevcdec's buffers at each change, so the block never writes motion vectors past one made at the smaller size.
  - A picture refused in pipelined mode no longer lets the next picture take the slice buffer of one the block is still reading.
  - Weighted prediction uses all 16 references (it stopped at 15).
  - `hevcdec_frame_to_i420_half`: 10-bit samples averaging 1022 or more stay 255 (they wrapped to 0, black), and halving to the frame's last chroma row with an odd height doesn't read below it.
  - **API:** `hevcdec_frame_to_i420` now returns an int, like `_16` and `_half`: HEVCDEC_OK, HEVCDEC_UNSUPPORTED for a window outside the frame (or an odd x or y), HEVCDEC_ERROR if the picture can't be finished. Code that ignores the result still compiles and works.
- **Once per picture, and without waiting (devkit 0.2.10, hevcdec 0.1.11):**
  - A frame is cleaned and invalidated once after the block writes it, not on every conversion call: sub-window, halved, history or sprite copies of the same picture pay for it once (about 1.3 ms for a 4K 10-bit frame on a Pi 4).
  - `hevcdec_frame_done(d, f)` says, without waiting, whether a picture is decoded (1) or still in the block (0). It also sees finished phases and starts the next. A player that has fallen behind can show a picture that's done and keep the one on screen rather than wait for one that isn't.
  - `hevcdec_stats` has three more fields at the end: `convert_waits` and `cs_convert_wait` (conversions that waited for their picture, and how long), and `cache_cleans`. Rebuild with the new header: the struct is bigger.
- **skip_frame (devkit 0.2.11):** the caller's `skip_frame` is passed to FFmpeg's HEVC decoder inside before each packet, so it can change as playback goes. At `AVDISCARD_NONREF` the pictures nothing refers to (sub-layer non-reference NAL types, and RASL pictures FFmpeg would skip) are dropped before they're parsed and **never reach the block**: a player that has fallen behind sets it so the block can get ahead, and clears it once caught up. The count is the read-only option `skipped` (`av_opt_get_int(avctx, "skipped", AV_OPT_SEARCH_CHILDREN, &n)`), also logged at close (verbose). Higher levels work too (`NONKEY` leaves only keyframes), but only NONREF pictures are counted.
- **Options:** `output_hw` (above), `drop_before` (above), `output_8bit` (default off; see 10-bit), `pipelined` (default on: the block decodes while FFmpeg parses the next picture; one picture is held back, so output is a picture later) and `cached_frames` (default on: the block's frames are cacheable, cleaned and invalidated before conversion, about twice as quick to convert).
- **Threads:** the decoder runs FFmpeg's HEVC decoder single-threaded; the parallelism is the block's.
- **Seeking:** `avcodec_flush_buffers` as for `hevc`.
- **Header:** hevcdec's header is `hwhevcdec.h` (devkit 0.2.3; 0.2.2 called it `hevcdec.h`, which clashed with FFmpeg's own `libavcodec/hevcdec.h`). It comes from `-I<devkit>/include`.
- **Speed (Pi 4, 1080p):** about 350 pictures a second through the block when pipelined, about 225 when not.

## vcdec on its own

See `vcdec.h`: open, send an access unit with its pts, receive a picture
into your planes (or take it without copying: `vcdec_receive_hold`, then
`vcdec_release`), flush, close. `vcdec_get_stats` says where the output
buffers are (pools or PCI memory) and where the time inside vcdec went.
Nothing waits for the decoder but open, flush and close, so it suits a
Wimp player polled on null events.

The GNU GPL version 2 (without "or later"), the same licence as Linux's
vchiq-mmal driver: vcdec is a new implementation over RISC OS's VCHIQ
module with its own MMAL client, based on the MMAL message formats as that
driver's headers define them (there's no published specification). It
contains none of the driver's code, but having used it as its reference
it is treated as derived from it. From riscos-reelhwaccel.

hevcdec is GPL version 2 too: it runs Linux's rpivid HEVC driver
(`rpivid_h265.c`, GPL-2.0-or-later) unchanged under a small shim, and
`hevc_hwdec` uses Raspberry Pi's FFmpeg code for filling the V4L2
controls (LGPL 2.1 or later). FFmpeg built with `--enable-libhevcdec` is
GPL.
