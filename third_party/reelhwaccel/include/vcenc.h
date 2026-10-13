/*
 * vcenc - H.264 encoded by the Raspberry Pi's VideoCore, from RISC OS,
 * through the VCHIQ module (in the ROM) and the firmware's MMAL service
 * (its ril.video_encode), as vcdec decodes. In libvcdec.
 *
 * For a program polled from Wimp null events: nothing here waits for the
 * encoder except open and close. Pictures go in as planar 4:2:0 (I420),
 * each with its pts; packets come out in the same order (the VideoCore's
 * encoder makes no B pictures), Annex B (start codes), each a whole
 * access unit.
 *
 *   vcenc_config c;
 *   vcenc_config_init(&c);
 *   c.width = 1280; c.height = 720; c.fps_num = 25; c.fps_den = 1; c.bitrate = 5000000;
 *   if (vcenc_open(&e, &c) != VCENC_OK) ...encode in software...
 *   for each picture:
 *     while vcenc_send(e, planes, strides, pts, 0) == VCENC_AGAIN: take packets (below)
 *     while vcenc_receive(e, &pkt) == VCENC_OK: write pkt.data, pkt.size
 *   at the end: vcenc_send_eos(e), then vcenc_receive until VCENC_EOF
 *   vcenc_close(e);
 *
 * The first packet is the stream's SPS and PPS on their own
 * (VCENC_PKT_CONFIG); vcenc_headers gives them too. With
 * VCENC_INLINE_HEADERS each keyframe's packet starts with them again (for
 * a stream that's joined part way; the firmware sends them as a buffer of
 * their own, which vcenc puts in front of the keyframe). Limits: 8-bit 4:2:0, up to 1920x1088,
 * even sizes; 1080p needs gpu_mem=128 or more.
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#ifndef VCENC_H
#define VCENC_H

#include <stddef.h>
#include <stdint.h>

#define VCENC_VERSION "0.1.3"

/* results (as vcdec's) */
#define VCENC_OK           0
#define VCENC_AGAIN        1   /* not now: send: no input buffer free (take packets, poll); receive: none yet */
#define VCENC_EOF          2   /* receive: every packet before the EOS has been taken */
#define VCENC_ERROR       -1   /* the encoder failed; vcenc_error says why. Only vcenc_close from here */
#define VCENC_UNSUPPORTED -2   /* not something the VideoCore encodes (size, ...): encode in software */
#define VCENC_EINVAL      -3   /* a bad call (vcenc_error says which); the encoder carries on */

#define VCENC_NOPTS INT64_MIN

/* vcenc_config.profile */
#define VCENC_PROFILE_HIGH      0   /* (the default) */
#define VCENC_PROFILE_MAIN      1
#define VCENC_PROFILE_BASELINE  2   /* constrained baseline */

/* vcenc_config.flags */
#define VCENC_INLINE_HEADERS  1u    /* SPS and PPS again at the start of every keyframe's packet */
#define VCENC_CBR             2u    /* constant bit rate (default: variable), tried once the encoder is
                                       running. The Pi 4's firmware refuses it every way, so there it's
                                       variable: vcenc_stats.rate_control says what took */
#define VCENC_LOG_MESSAGES    4u    /* every MMAL message to the log */
#define VCENC_NO_GPU_MEM_CHECK 8u   /* open even if gpu_mem looks too small for the size */
#define VCENC_PCI            16u    /* buffers in PCI_RAMAlloc memory, not Physical Memory Pools */

/* vcenc_send's flags */
#define VCENC_FORCE_KEY       1u    /* this picture a keyframe (IDR) */

/* vcenc_packet.flags */
#define VCENC_PKT_KEY         1u    /* a keyframe (IDR) */
#define VCENC_PKT_CONFIG      2u    /* SPS and PPS only (the first packet) */

typedef struct vcenc vcenc;

typedef struct {
    int width, height;          /* the pictures' size: even, up to 1920x1088 */
    int fps_num, fps_den;       /* frames a second (0: 25/1) */
    unsigned bitrate;           /* bits a second (0: 10 Mbit/s) */
    int profile;                /* VCENC_PROFILE_... */
    int level;                  /* H.264 level times 10, e.g. 40, 41 (0: the encoder's choice) */
    int gop;                    /* pictures from one keyframe to the next (0: the encoder's default) */
    int qp_min, qp_max;         /* quantiser limits (0: the encoder's) */
    unsigned flags;             /* VCENC_INLINE_HEADERS, ... */
    int in_buffers;             /* input pictures with the encoder at once (0: 2; at most 4) */
    void (*log)(void *handle, const char *text);   /* or NULL */
    void *log_handle;
} vcenc_config;

typedef struct {
    const uint8_t *data;        /* valid until the next vcenc_receive or vcenc_close */
    size_t size;
    int64_t pts, dts;           /* the picture's pts (dts the same: no reordering) */
    unsigned flags;             /* VCENC_PKT_KEY, VCENC_PKT_CONFIG */
} vcenc_packet;

typedef struct {
    unsigned pictures, packets, keyframes, config_packets;
    unsigned long long bytes;   /* packet bytes out */
    unsigned cs_copy;           /* copying pictures into the input buffers, centiseconds in all */
    unsigned cs_wait;           /* send waiting in vcenc_send_eos / close */
    int in_pools, out_pools;    /* buffers in Physical Memory Pools (else PCI memory) */
    unsigned in_size, out_size, out_buffers;   /* buffer sizes and the output's count, as set */
    unsigned app_page_moves;    /* claiming memory moved the program's page at &8000 (should be 0) */
    int rate_control;           /* (0.1.1) as set: 1 variable, 2 constant, 4 constant skipping frames
                                   (0.1.2: either of the last two possibly only once running) */
    int rate_control_running;   /* (0.1.2) constant was set once the encoder was running */
    unsigned headers_repeated;  /* (0.1.1) keyframe packets with SPS and PPS put in front */
} vcenc_stats;

void vcenc_config_init(vcenc_config *c);
int vcenc_open(vcenc **out, const vcenc_config *c);
const char *vcenc_open_error(void);
const char *vcenc_error(const vcenc *e);

/* a picture: planes[0..2] Y, U, V (U and V (w+1)/2 x (h+1)/2), strides in
   bytes. VCENC_AGAIN: no input buffer free yet (packets are waiting to be
   taken, or the encoder is busy): take packets and try again. */
int vcenc_send(vcenc *e, const uint8_t *const planes[3], const int strides[3], int64_t pts, unsigned flags);
int vcenc_send_eos(vcenc *e);

/* the next packet: VCENC_OK, VCENC_AGAIN, or VCENC_EOF after the EOS */
int vcenc_receive(vcenc *e, vcenc_packet *pkt);

/* SPS and PPS (Annex B), once the encoder has given them: VCENC_OK, or
   VCENC_AGAIN (receive more first) */
int vcenc_headers(vcenc *e, const uint8_t **data, size_t *size);

/* takes in whatever the encoder has sent (send and receive do this too) */
int vcenc_poll(vcenc *e);

void vcenc_get_stats(const vcenc *e, vcenc_stats *s);
void vcenc_close(vcenc *e);

#endif
