/*
 * hevcdec - HEVC (H.265) decoded by the Raspberry Pi 4's own HEVC block,
 * from RISC OS. A stateless decoder: the caller parses the stream (as
 * FFmpeg's HEVC decoder does) and gives each picture's parameters as the
 * V4L2 stateless HEVC controls (hevc_ctrls.h), its slices, and a frame to
 * decode into; hevcdec builds the block's commands (Raspberry Pi's
 * rpivid_h265.c, unchanged) and runs its two phases.
 *
 *   hevcdec_config c; hevcdec_config_init(&c); c.width = 1920; c.height = 1080;
 *   hevcdec_open(&d, &c);
 *   f = hevcdec_frame_new(d);                      (as many as the DPB needs + 1)
 *   for each picture in decoding order:
 *     hevcdec_decode(d, &pic, f, number);         (number: how later pictures'
 *                                                  DPB entries name this one)
 *     hevcdec_frame_to_i420(d, f, planes, strides)
 *   hevcdec_close(d);
 *
 * 0.1.9: hevcdec_frame_to_i420_half (straight into a half-size overlay)
 * and hevcdec_frame_decoder (frames handed out unconverted by hevc_hwdec).
 * 0.1.8: the block kept busy while frames are converted (phases seen
 * finishing between columns); 10-bit rows unpacked straight into the
 * planes; hevcdec_convert_benchmark.
 * 0.1.7: 10-bit too (config.bit_depth; frames NV12_10_COL128, converted to
 * 16-bit samples by hevcdec_frame_to_i420_16, or to 8-bit), and sizes up
 * to 4096x4096 (4K) tested.
 * 0.1.5: 8-bit 4:2:0 only; output frames cacheable (unless the config
 * says not), converted with NEON; pictures one at a time, or pipelined
 * (config.pipelined: hevcdec_decode returns at once, hevcdec_frame_wait). Streams without scaling
 * lists are given flat ones (the block needs its factors loaded).
 *
 * Part of riscos-reelhwaccel. GPL version 2 (see COPYING).
 */
#ifndef HWHEVCDEC_H
#define HWHEVCDEC_H

#include <stddef.h>
#include <stdint.h>
#include "hevc_ctrls.h"

#define HEVCDEC_VERSION "0.1.9"

#define HEVCDEC_OK           0
#define HEVCDEC_ERROR       -1   /* hevcdec_error says why */
#define HEVCDEC_UNSUPPORTED -2   /* not for the block (size, depth, chroma format) */

typedef struct hevcdec hevcdec;
typedef struct hevcdec_frame hevcdec_frame;

typedef struct {
    int width, height;          /* the largest picture to come (frames are made this size) */
    int bit_depth;              /* 8 (default) or 10 (0.1.7): the stream's; frames are made for it */
    int cached_frames;          /* 1 (default): output frames cacheable, much quicker to read */
    int pipelined;              /* 0 (default): hevcdec_decode waits for its picture. 1: it returns
                                   once the picture is given to the block (hevcdec_frame_wait for it),
                                   so the block decodes while the program works (phase 1 of the next
                                   picture alongside phase 2 of the last) */
    void (*log)(void *handle, const char *text);
    void *log_handle;
} hevcdec_config;

typedef struct {
    const struct v4l2_ctrl_hevc_slice_params *params;
    const uint8_t *data;        /* the slice's NAL unit, emulation prevention bytes kept */
    size_t size;
} hevcdec_slice;

typedef struct {
    const struct v4l2_ctrl_hevc_sps *sps;
    const struct v4l2_ctrl_hevc_pps *pps;
    const struct v4l2_ctrl_hevc_decode_params *dec;    /* dpb[].timestamp: the numbers given to hevcdec_decode */
    const struct v4l2_ctrl_hevc_scaling_matrix *scaling;  /* or NULL */
    unsigned int nslices;
    const hevcdec_slice *slices;
} hevcdec_picture;

typedef struct {
    unsigned pictures, phase1_retries;
    unsigned cs_phase1, cs_phase2;      /* waiting for each phase, centiseconds in all */
    unsigned cs_cache;                  /* cleaning and invalidating cached frames before reading */
    unsigned cs_wait;                   /* the program waiting for the block, in all */
    int cached_frames;                  /* the frames are cacheable */
    /* buffers the block wrote past the end of (each has a guard area after
       it, so no harm done), the most bytes past any, and which that was */
    unsigned overruns, overrun_max;
    const char *overrun_what;
    /* (0.1.6) times claiming the block's memory moved the program's page
       at &8000 to another physical page: should be 0 (ARMEABISupport finds
       the program by that page) */
    unsigned app_page_moves;
    /* (0.1.8) times the program had to wait for the block (cs_wait's count) */
    unsigned waits;
} hevcdec_stats;

void hevcdec_config_init(hevcdec_config *c);
int hevcdec_open(hevcdec **out, const hevcdec_config *c);
const char *hevcdec_open_error(void);

/* An output frame (NV12, or 10-bit NV12 three samples a word, in 128-byte
   columns, as the block writes it); NULL
   if there's no memory. Frames go with hevcdec_close. */
hevcdec_frame *hevcdec_frame_new(hevcdec *d);

/* Decodes one picture into f: waiting for the block, or (pipelined) only
   until it's given to it. f may be a frame still being decoded or one a
   later picture no longer refers to (it's waited for first). HEVCDEC_OK,
   or HEVCDEC_ERROR (hevcdec_error; the decoder can carry on with the next
   IRAP picture), or HEVCDEC_UNSUPPORTED. */
int hevcdec_decode(hevcdec *d, const hevcdec_picture *pic, hevcdec_frame *f, uint64_t number);

/* Waits for f's picture (pipelined): HEVCDEC_OK when it's decoded, or
   HEVCDEC_ERROR (the block failed it, or the decoder stopped). */
int hevcdec_frame_wait(hevcdec *d, hevcdec_frame *f);
/* Waits for every picture given (before a seek, say) */
int hevcdec_finish(hevcdec *d);

/* Part of the frame's picture as planar 8-bit 4:2:0: w x h from (x, y) in
   luma samples (the SPS's output window; x and y even), U and V
   (w+1)/2 x (h+1)/2. From a 10-bit decoder: each sample's top 8 bits
   (truncated, not rounded or dithered). */
void hevcdec_frame_to_i420(hevcdec *d, const hevcdec_frame *f, uint8_t *const planes[3], const int strides[3], int x,
                           int y, int w, int h);
/* (0.1.7) The same as 10-bit samples in 16-bit words (FFmpeg's
   AV_PIX_FMT_YUV420P10 on a little-endian machine; strides in bytes, planes
   2-byte aligned): a 10-bit decoder only (else HEVCDEC_UNSUPPORTED) */
int hevcdec_frame_to_i420_16(hevcdec *d, const hevcdec_frame *f, uint16_t *const planes[3], const int strides[3], int x,
                             int y, int w, int h);
/* (0.1.8) While a frame is converted, hevcdec sees phases finish and
   starts the next (pipelined: the block decodes the next picture while
   the program copies this one).
   For measuring (HEVCTest -K): f converted n times the given way (0
   column by column, the default; 1 row by row; 2 column by column with
   preloading; 3 10-bit through a row buffer, as 0.1.7), to bits 8 or 16
   (16: a 10-bit decoder), each time as hevcdec_frame_to_i420(_16) does it
   (the cache cleaned and invalidated first); the centiseconds taken in
   *cs. HEVCDEC_OK, or HEVCDEC_UNSUPPORTED. */
/* (0.1.9) The decoder a frame belongs to (for a caller handed frames by
   FFmpeg's hevc_hwdec with output_hw: data[3] is the hevcdec_frame) */
hevcdec *hevcdec_frame_decoder(const hevcdec_frame *f);
/* (0.1.9) The window w*2 x h*2 from (x, y) halved into w x h (U and V
   (w+1)/2 x (h+1)/2) of planar 8-bit 4:2:0: each sample the rounded mean
   of a 2x2 block (a 4K picture into an HD-sized overlay, in one pass);
   from a 10-bit decoder, the means rounded to 8 bits. x a multiple of 4, y
   even. HEVCDEC_OK, or HEVCDEC_UNSUPPORTED (outside the frame). */
int hevcdec_frame_to_i420_half(hevcdec *d, const hevcdec_frame *f, uint8_t *const planes[3], const int strides[3], int x,
                               int y, int w, int h);
#define HEVCDEC_CONVERT_WAYS 4
int hevcdec_convert_benchmark(hevcdec *d, const hevcdec_frame *f, void *const planes[3], const int strides[3], int bits,
                              int x, int y, int w, int h, int way, int n, unsigned *cs);

const char *hevcdec_error(const hevcdec *d);
void hevcdec_get_stats(const hevcdec *d, hevcdec_stats *s);
void hevcdec_close(hevcdec *d);

#endif
