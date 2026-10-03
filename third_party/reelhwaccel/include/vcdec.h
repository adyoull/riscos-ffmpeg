/*
 * vcdec - H.264 decoded by the Raspberry Pi's VideoCore, from RISC OS,
 * through the VCHIQ module (in the ROM) and the firmware's MMAL service.
 *
 * For a player polled from Wimp null events: nothing here waits for the
 * decoder except open, flush and close (a few centiseconds each on a Pi 4)
 * and the rare format change. Pictures come back in display order, each
 * with the pts it was sent with.
 *
 *   vcdec_config c;
 *   vcdec_config_init(&c);
 *   c.width = 1920; c.height = 1080;
 *   if (vcdec_open(&d, &c) == VCDEC_UNSUPPORTED) ...decode in software...
 *   on each null event:
 *     while there's a packet and vcdec_send(d, data, len, pts, dts, key) == VCDEC_OK: next packet
 *     while vcdec_receive(d, &pic, planes, strides) == VCDEC_OK: show pic
 *   at the end of the file: vcdec_send_eos(d), until vcdec_receive gives VCDEC_EOF
 *   to seek: vcdec_flush(d), then send from a keyframe
 *   vcdec_close(d);
 *
 * The stream must be H.264 Annex B (start codes, SPS and PPS in band before
 * each IDR, as FFmpeg's h264_mp4toannexb makes from an MP4), one access unit
 * a call. Limits: Main, High or Baseline profile (8-bit 4:2:0), up to
 * 1920x1088; 1080p needs gpu_mem=128 or more in CONFIG/TXT (at 64 the
 * decoder stalls without a word), which vcdec_open checks.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#ifndef VCDEC_H
#define VCDEC_H

#include <stddef.h>
#include <stdint.h>

#define VCDEC_VERSION "0.4.2"

/* results */
#define VCDEC_OK           0
#define VCDEC_AGAIN        1   /* not now: poll again later (send: input full; receive: no picture yet) */
#define VCDEC_EOF          2   /* receive: every picture before the EOS has been taken */
#define VCDEC_ERROR       -1   /* the decoder failed; vcdec_error says why. Only vcdec_close from here */
#define VCDEC_UNSUPPORTED -2   /* this stream or size isn't for the VideoCore: decode it in software */
#define VCDEC_EINVAL      -3   /* a bad call (vcdec_error says which); the decoder carries on */

#define VCDEC_NOPTS INT64_MIN  /* no time (MMAL's TIME_UNKNOWN has the same bits) */

/* vcdec_config.flags */
#define VCDEC_NO_GPU_MEM_CHECK 1u   /* open even if gpu_mem looks too small for the size */
#define VCDEC_SYNC_RECEIVE     2u   /* wait for each picture's bulk transfer as it arrives
                                       (as MMALDecode did) instead of letting it run on */
#define VCDEC_LOG_MESSAGES     4u   /* every MMAL message to the log */
/* how pictures are copied out (default: LDM/STM eight words a loop) */
#define VCDEC_COPY_LDM8        8u   /* LDM/STM eight words a loop (the default since 0.4) */
#define VCDEC_COPY_NEON       16u   /* NEON, 64 bytes a loop (only from a user readable pool;
                                       otherwise LDM 8: NEON isn't used in SVC mode) */
#define VCDEC_COPY_LDM4      256u   /* LDM/STM four words a loop (the default before 0.4) */
/* where pictures arrive. Default (since 0.4): a cacheable Physical Memory
   Pool of contiguous pages below 1 GB, user readable (the cache cleaned and
   invalidated over each picture before it's read); if a pool can't be had
   (RISC OS before 5.23, no contiguous pages), PCI_RAMAlloc memory instead,
   said in the log and in vcdec_stats */
#define VCDEC_OUT_PMP         32u   /* a pool or nothing: no falling back to PCI memory */
#define VCDEC_OUT_UNCACHED    64u   /* the pool not cacheable (bufferable) */
#define VCDEC_OUT_PCI        128u   /* PCI_RAMAlloc memory (uncachable, privileged: copied in
                                       SVC mode), the default before 0.4 */

/* vcdec_send's flags */
#define VCDEC_KEYFRAME      1u
#define VCDEC_DISCONTINUITY 2u      /* (the decoder echoes it on the picture) */

/* vcdec_picture.flags */
#define VCDEC_PIC_KEYFRAME      1u
#define VCDEC_PIC_DISCONTINUITY 2u

typedef struct vcdec vcdec;
typedef struct vcdec_hold vcdec_hold;

typedef struct {
    int width, height;          /* the stream's size (from the container): needed */
    unsigned flags;             /* VCDEC_NO_GPU_MEM_CHECK, ... */
    int out_buffers;            /* output buffers: 0 for 3, up to 16 (with vcdec_receive_hold: 3 + the
                                   most pictures the caller holds at once) */
    void (*log)(void *handle, const char *text);   /* optional: what the decoder does */
    void *log_handle;
} vcdec_config;

typedef struct {
    int width, height;          /* the visible picture */
    int64_t pts;                /* as sent (microseconds or any unit), or VCDEC_NOPTS */
    unsigned flags;             /* VCDEC_PIC_... */
} vcdec_picture;

typedef struct {
    unsigned sent, pictures, discarded, format_changes, flushes, recreated;
    /* where the output buffers are now (0.4): in pools, and in PCI memory */
    unsigned pool_buffers, pci_buffers;
    /* where vcdec_receive's time goes (0.4), in centiseconds in all (each
       part timed by OS_ReadMonotonicTime, so a total over many pictures):
       taking in the decoder's messages (queueing the pictures' transfers
       among them), the transfers' queueing alone, handing buffers back,
       the cache cleaned and invalidated, and the copy */
    unsigned cs_messages, cs_queue, cs_give, cs_cache, cs_copy;
    /* (0.4.1) pictures taken by vcdec_receive_hold, and held now */
    unsigned holds, held_now;
    /* (0.4.2) times claiming a pool's pages moved the program's page at
       &8000 to another physical page: should be 0 (ARMEABISupport finds
       the program by that page) */
    unsigned app_page_moves;
} vcdec_stats;

void vcdec_config_init(vcdec_config *c);

/* VCDEC_OK and *out, or VCDEC_UNSUPPORTED / VCDEC_ERROR (*out NULL; the
   reason in vcdec_open_error()). */
int vcdec_open(vcdec **out, const vcdec_config *c);
const char *vcdec_open_error(void);

/* One access unit (Annex B), copied: the caller's buffer is free on return.
   VCDEC_AGAIN: the decoder's input is full; try again after a poll.
   VCDEC_UNSUPPORTED: its SPS names a profile the VideoCore can't decode. */
int vcdec_send(vcdec *d, const void *data, size_t len, int64_t pts, int64_t dts, unsigned flags);

/* The end of the stream: only when the player really is at the end (a flush
   after it costs the decoder a restart). VCDEC_AGAIN if the input is full. */
int vcdec_send_eos(vcdec *d);

/* Takes in what the decoder has sent. send, peek and receive do this too. */
int vcdec_poll(vcdec *d);

/* The next picture's size and pts, without taking it: VCDEC_OK, VCDEC_AGAIN
   or VCDEC_EOF. */
int vcdec_peek(vcdec *d, vcdec_picture *pic);

/* The next picture, copied into the caller's three planes (Y, U, V: I420,
   strides in bytes; U and V are (width+1)/2 x (height+1)/2). planes NULL:
   the picture is dropped. VCDEC_OK, VCDEC_AGAIN or VCDEC_EOF. */
int vcdec_receive(vcdec *d, vcdec_picture *pic, uint8_t *const planes[3], const int strides[3]);

/* (0.4.1) The next picture without copying it: planes and strides point
   into vcdec's own buffer (I420, user readable, the cache already made
   right), read only, valid until vcdec_release. VCDEC_OK and *hold,
   VCDEC_AGAIN or VCDEC_EOF as vcdec_receive; or VCDEC_UNSUPPORTED when
   this picture can't be held (its buffer is PCI memory, or the caller
   already holds all but 2 of the output buffers): take it with
   vcdec_receive instead. A held buffer isn't the decoder's again until
   it's released (at the next call after), so give it enough
   (vcdec_config.out_buffers). */
int vcdec_receive_hold(vcdec *d, vcdec_picture *pic, uint8_t *planes[3], int strides[3], vcdec_hold **hold);

/* A held picture given back: exactly once for each hold (a hold is
   reused for its buffer's next picture). Also after vcdec_close: a closed
   decoder's last memory goes with its last hold. NULL is ignored. It
   sends nothing to the decoder, but isn't safe against the other calls
   from another thread. */
void vcdec_release(vcdec_hold *hold);

/* Everything sent so far forgotten, and every picture not yet taken
   dropped (a seek): then send from a keyframe. After an EOS, the decoder is
   created again (a flush alone would lose pictures at the next EOS). */
int vcdec_flush(vcdec *d);

/* Pictures still held stay valid until released. */
void vcdec_close(vcdec *d);

/* Why the last call failed ("" if none). */
const char *vcdec_error(const vcdec *d);

void vcdec_get_stats(const vcdec *d, vcdec_stats *s);

/* The VideoCore's memory (gpu_mem) in MB, or 0 if the firmware doesn't say. */
unsigned vcdec_gpu_mem(void);

/* Times copying one picture's worth of the first output buffer into the
   caller's planes, reps times, as vcdec_receive would (`way`:
   VCDEC_COPY_LDM4, VCDEC_COPY_LDM8 or VCDEC_COPY_NEON (0: LDM 8); a cached pool is invalidated before
   each copy, as after a transfer). Needs a picture to have been received.
   VCDEC_OK and the time in centiseconds, or VCDEC_EINVAL. */
int vcdec_copy_benchmark(vcdec *d, unsigned way, int reps, uint8_t *const planes[3], const int strides[3],
                         unsigned *cs);

/* VCDEC_OK if an access unit's SPS (if it has one) is a profile the
   VideoCore decodes, VCDEC_UNSUPPORTED if not. */
int vcdec_check_stream(const void *data, size_t len);

#endif
