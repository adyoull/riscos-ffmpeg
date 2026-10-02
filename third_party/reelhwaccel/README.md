# riscos-reelhwaccel devkit

What FFmpeg and Reel need to decode H.264 on the Raspberry Pi's VideoCore
from RISC OS:

| File | What it is |
|---|---|
| `include/vcdec.h`, `lib/libvcdec.a` | vcdec: the VideoCore's H.264 decoder through RISC OS's VCHIQ module and the firmware's MMAL service. GCCSDK GCC 10, ARMv7, hard float. |
| `ffmpeg/0001-avcodec-h264_vchiq.patch` | FFmpeg 5.1.10's `h264_vchiq` decoder on top of vcdec (`git am`, or `patch -p1`). It applies to plain 5.1.10 and on top of riscos-ffmpeg's series. |

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
- **Linking:** add `-lvcdec` after `-lavcodec` when linking programs.

## vcdec on its own

See `vcdec.h`: open, send an access unit with its pts, receive a picture
into your planes (or take it without copying: `vcdec_receive_hold`, then
`vcdec_release`), flush, close. `vcdec_get_stats` says where the output
buffers are (pools or PCI memory) and where the time inside vcdec went.
Nothing waits for the decoder but open, flush and close, so it suits a
Wimp player polled on null events.

GPL version 2 or later. From riscos-reelhwaccel.
