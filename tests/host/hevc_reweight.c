/*
 * Makes an HEVC stream for hevc_weights_test and the framemd5 check: the
 * weight tables of a weighted clip rewritten (with FFmpeg's CBS) so that each
 * term of patch 0024's "all default" test decides some slice on its own.
 * Encoders don't make most of these (x265 never sends a chroma offset without
 * a luma weight, for one), so they are made here.  Per slice, from a fixed
 * pseudo-random sequence: a third of slices all default (some with the
 * defaults sent explicitly), the rest default but for one reference (L0 or,
 * for B, L1) given one of: a luma offset, a luma weight, a chroma offset, a
 * chroma weight (sent with the offset that leaves its chroma offset 0).
 * Writes Annex B (parameter sets first).
 * Usage: hevc_reweight in.mkv out.hevc
 */
#include <stdio.h>
#include "libavformat/avformat.h"
#include "libavcodec/cbs.h"
#include "libavcodec/cbs_h265.h"

static unsigned st = 7;
static unsigned rnd(unsigned n) { st = st * 1103515245u + 12345u; return (st >> 8) % n; }

static void ref_default(H265RawSliceHeader *h, int l, int i, int explicit)
{
    if (!l) {
        h->luma_weight_l0_flag[i] = h->chroma_weight_l0_flag[i] = explicit;
        h->delta_luma_weight_l0[i] = h->luma_offset_l0[i] = 0;
        h->delta_chroma_weight_l0[i][0] = h->delta_chroma_weight_l0[i][1] = 0;
        h->chroma_offset_l0[i][0] = h->chroma_offset_l0[i][1] = 0;
    } else {
        h->luma_weight_l1_flag[i] = h->chroma_weight_l1_flag[i] = explicit;
        h->delta_luma_weight_l1[i] = h->luma_offset_l1[i] = 0;
        h->delta_chroma_weight_l1[i][0] = h->delta_chroma_weight_l1[i][1] = 0;
        h->chroma_offset_l1[i][0] = h->chroma_offset_l1[i][1] = 0;
    }
}

static void ref_change(H265RawSliceHeader *h, int l, int i, int kind, int *kinds)
{
    int v = (int)rnd(20) - 10, j = rnd(2);
    int cd = h->luma_log2_weight_denom + h->delta_chroma_log2_weight_denom;
    if (!v) v = 3;
    kinds[kind]++;
    switch (kind) {
    case 0: if (!l) { h->luma_weight_l0_flag[i] = 1; h->luma_offset_l0[i] = v; }
            else    { h->luma_weight_l1_flag[i] = 1; h->luma_offset_l1[i] = v; } break;
    case 1: if (!l) { h->luma_weight_l0_flag[i] = 1; h->delta_luma_weight_l0[i] = v; }
            else    { h->luma_weight_l1_flag[i] = 1; h->delta_luma_weight_l1[i] = v; } break;
    case 2: if (!l) { h->chroma_weight_l0_flag[i] = 1; h->chroma_offset_l0[i][j] = v * 4; }
            else    { h->chroma_weight_l1_flag[i] = 1; h->chroma_offset_l1[i][j] = v * 4; } break;
    case 3: /* the offset sent cancels the one a weight brings (offset = sent - (128 * w >> denom) + 128),
             * so only the weight differs */
            if (128 * v >> cd > 511 || 128 * v >> cd < -512) v = v < 0 ? -1 : 1;
            if (!l) { h->chroma_weight_l0_flag[i] = 1; h->delta_chroma_weight_l0[i][j] = v; h->chroma_offset_l0[i][j] = 128 * v >> cd; }
            else    { h->chroma_weight_l1_flag[i] = 1; h->delta_chroma_weight_l1[i][j] = v; h->chroma_offset_l1[i][j] = 128 * v >> cd; } break;
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fc = NULL;
    CodedBitstreamContext *cbs;
    CodedBitstreamFragment frag = { 0 };
    CodedBitstreamH265Context *h265;
    AVPacket *pkt = av_packet_alloc();
    FILE *out;
    int vi, u, kinds[4] = { 0 }, all_default = 0;

    if (argc < 3 || avformat_open_input(&fc, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0)
        return 1;
    vi = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ff_cbs_init(&cbs, AV_CODEC_ID_HEVC, NULL) < 0 || !(out = fopen(argv[2], "wb")))
        return 1;
    h265 = cbs->priv_data;
    if (ff_cbs_read_extradata(cbs, &frag, fc->streams[vi]->codecpar) < 0 ||
        ff_cbs_write_fragment_data(cbs, &frag) < 0)
        return 1;
    fwrite(frag.data, 1, frag.data_size, out);
    ff_cbs_fragment_reset(&frag);
    while (av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vi) {
            if (ff_cbs_read_packet(cbs, &frag, pkt) < 0) { fprintf(stderr, "read failed\n"); return 1; }
            for (u = 0; u < frag.nb_units; u++) {
                H265RawSliceHeader *h;
                const H265RawPPS *pps = h265->active_pps;
                int nl0, nl1, i, l;
                if (frag.units[u].type > HEVC_NAL_CRA_NUT || !frag.units[u].content) continue;
                h = &((H265RawSlice *)frag.units[u].content)->header;
                if (!((h->slice_type == HEVC_SLICE_P && pps->weighted_pred_flag) ||
                      (h->slice_type == HEVC_SLICE_B && pps->weighted_bipred_flag)))
                    continue;
                nl0 = h->num_ref_idx_active_override_flag ? h->num_ref_idx_l0_active_minus1 + 1
                                                          : pps->num_ref_idx_l0_default_active_minus1 + 1;
                nl1 = h->slice_type != HEVC_SLICE_B ? 0 :
                      h->num_ref_idx_active_override_flag ? h->num_ref_idx_l1_active_minus1 + 1
                                                          : pps->num_ref_idx_l1_default_active_minus1 + 1;
                for (l = 0; l < 2; l++)
                    for (i = 0; i < (l ? nl1 : nl0); i++)
                        ref_default(h, l, i, rnd(2));
                if (rnd(3) == 0) { all_default++; continue; }
                l = nl1 && rnd(2);
                ref_change(h, l, rnd(l ? nl1 : nl0), rnd(4), kinds);
            }
            if (ff_cbs_write_fragment_data(cbs, &frag) < 0) { fprintf(stderr, "write failed\n"); return 1; }
            fwrite(frag.data, 1, frag.data_size, out);
            ff_cbs_fragment_reset(&frag);
        }
        av_packet_unref(pkt);
    }
    fclose(out);
    printf("  rewritten: %d slices all default; one reference changed: %d luma offset, %d luma weight, "
           "%d chroma offset, %d chroma weight\n", all_default, kinds[0], kinds[1], kinds[2], kinds[3]);
    ff_cbs_fragment_free(&frag);
    ff_cbs_close(&cbs);
    avformat_close_input(&fc);
    av_packet_free(&pkt);
    return 0;
}
