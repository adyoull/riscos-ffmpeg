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
- **Options:** `sync_receive` (wait for each picture's transfer as it arrives) and `gpu_mem_check` (default on).
- **Linking:** add `-lvcdec` after `-lavcodec` when linking programs.

## vcdec on its own

See `vcdec.h`: open, send an access unit with its pts, receive a picture
into your planes, flush, close. Nothing waits for the decoder but open,
flush and close, so it suits a Wimp player polled on null events.

GPL version 2 or later. From riscos-reelhwaccel.
