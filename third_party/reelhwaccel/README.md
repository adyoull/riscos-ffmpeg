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
(converted from the block's 128-byte column format), with their pts and
cropping applied. Ask for it by name (`-c:v hevc_hwdec`, or
`avcodec_find_decoder_by_name("hevc_hwdec")`).

- **Refused with AVERROR(ENOSYS),** so the caller can open `hevc` instead: anything but 8-bit 4:2:0 (10-bit is still to come); over 4096x4096; a picture size that grows part way; machines without the block (not a Pi 4). Streams with their parameter sets in the extradata (MP4, MKV) are refused at open; raw streams at their first picture.
- **A picture the block fails on** comes out with `decode_error_flags` set (FFmpeg logs "corrupt decoded frame"); decoding carries on.
- **Options:** `pipelined` (default on: the block decodes while FFmpeg parses the next picture; one picture is held back, so output is a picture later) and `cached_frames` (default on: the block's frames are cacheable, cleaned and invalidated before conversion, about twice as quick to convert).
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
