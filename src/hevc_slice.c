/* hevc_slice.c -- slice segment header (I-slice / HEIC still focus) */
#include "heic_internal.h"

static int ceil_log2(uint32_t x)
{
    int n = 0;
    if (x <= 1) return 0;
    x--;
    while (x) {
        x >>= 1;
        n++;
    }
    return n;
}

static int nal_is_idr(heic_nal_type t)
{
    return t == HEIC_NAL_IDR_W_RADL || t == HEIC_NAL_IDR_N_LP;
}

static int nal_is_irap(heic_nal_type t)
{
    return t >= HEIC_NAL_BLA_W_LP && t <= HEIC_NAL_CRA;
}

int heic_parse_slice_header(heic_ctx *ctx, const heic_nal *nal,
                            const heic_sps *sps, const heic_pps *pps,
                            heic_slice_header *out)
{
    heic_bs bs;
    uint32_t st;
    int i;

    memset(out, 0, sizeof(*out));
    if (!nal || !sps || !pps || !out) return -1;
    heic_bs_init(&bs, nal->payload, nal->payload_len);

    out->first_slice_segment_in_pic_flag = heic_bs_bit(&bs);
    if (nal_is_irap(nal->type))
        out->no_output_of_prior_pics_flag = heic_bs_bit(&bs);

    out->pps_id = (uint8_t)heic_bs_ue(&bs);
    if (out->pps_id != pps->pps_pic_parameter_set_id) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "slice PPS id mismatch");
        return -1;
    }

    if (!out->first_slice_segment_in_pic_flag) {
        if (pps->dependent_slice_segments_enabled_flag)
            out->dependent_slice_segment_flag = heic_bs_bit(&bs);
        {
            int bits = ceil_log2(sps->pic_size_in_ctbs);
            out->slice_segment_address = heic_bs_bits(&bs, bits);
        }
    }
    if (out->dependent_slice_segment_flag) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "dependent slice segments not supported");
        return -1;
    }

    for (i = 0; i < pps->num_extra_slice_header_bits; i++)
        (void)heic_bs_bit(&bs);

    st = heic_bs_ue(&bs);
    if (st > 2) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "invalid slice type");
        return -1;
    }
    out->slice_type = (int)st;

    out->pic_output_flag = pps->output_flag_present_flag ? heic_bs_bit(&bs) : 1;
    if (sps->separate_colour_plane_flag)
        out->colour_plane_id = (uint8_t)heic_bs_bits(&bs, 2);

    if (!nal_is_idr(nal->type)) {
        int poc_bits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
        out->slice_pic_order_cnt_lsb = heic_bs_bits(&bs, poc_bits);
        /* short_term_ref_pic_set / long-term / temporal MVP — skip with minimal
           parsing for non-IDR stills (rare in HEIC). Full RPS later. */
        {
            int short_term_ref_pic_set_sps_flag = heic_bs_bit(&bs);
            if (!short_term_ref_pic_set_sps_flag) {
                heic_error(ctx, HEIC_SEVERITY_ERROR,
                           "inline short-term RPS not yet parsed");
                return -1;
            }
            /* If num_short_term_ref_pic_sets > 1 would read index; SPS path
               currently doesn't store num_st_rps count for selection. OK when 0/1. */
        }
        if (sps->long_term_ref_pics_present_flag) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "long-term RPS not yet parsed");
            return -1;
        }
        if (sps->sps_temporal_mvp_enabled_flag)
            (void)heic_bs_bit(&bs); /* slice_temporal_mvp_enabled_flag */
    }

    if (sps->sample_adaptive_offset_enabled_flag) {
        out->slice_sao_luma_flag = heic_bs_bit(&bs);
        if (sps->chroma_format_idc != 0)
            out->slice_sao_chroma_flag = heic_bs_bit(&bs);
    }

    if (out->slice_type != HEIC_SLICE_I) {
        /* Inter slice fields — HEIC stills are I-only; reject for now. */
        heic_error(ctx, HEIC_SEVERITY_ERROR, "P/B slices not yet supported");
        return -1;
    }

    out->slice_qp_delta = (int8_t)heic_bs_se(&bs);
    if (pps->pps_slice_chroma_qp_offsets_present_flag) {
        out->slice_cb_qp_offset = (int8_t)heic_bs_se(&bs);
        out->slice_cr_qp_offset = (int8_t)heic_bs_se(&bs);
    }

    if (pps->deblocking_filter_override_enabled_flag)
        out->deblocking_filter_override_flag = heic_bs_bit(&bs);
    if (out->deblocking_filter_override_flag) {
        out->slice_deblocking_filter_disabled_flag = heic_bs_bit(&bs);
        if (!out->slice_deblocking_filter_disabled_flag) {
            out->slice_beta_offset_div2 = (int8_t)heic_bs_se(&bs);
            out->slice_tc_offset_div2 = (int8_t)heic_bs_se(&bs);
        }
    } else {
        out->slice_deblocking_filter_disabled_flag =
            pps->pps_deblocking_filter_disabled_flag;
        out->slice_beta_offset_div2 = pps->pps_beta_offset_div2;
        out->slice_tc_offset_div2 = pps->pps_tc_offset_div2;
    }

    if (pps->pps_loop_filter_across_slices_enabled_flag &&
        (out->slice_sao_luma_flag || out->slice_sao_chroma_flag ||
         !out->slice_deblocking_filter_disabled_flag))
        out->slice_loop_filter_across_slices_enabled_flag = heic_bs_bit(&bs);
    else
        out->slice_loop_filter_across_slices_enabled_flag =
            pps->pps_loop_filter_across_slices_enabled_flag;

    if (pps->tiles_enabled_flag || pps->entropy_coding_sync_enabled_flag) {
        out->num_entry_point_offsets = heic_bs_ue(&bs);
        if (out->num_entry_point_offsets > 0) {
            uint32_t offset_len_minus1 = heic_bs_ue(&bs);
            uint32_t e;
            /* Cap entry points: stack entry_cum[4096] and fuzz budget. */
            if (offset_len_minus1 > 31 || out->num_entry_point_offsets > 4096)
                return -1;
            out->entry_point_offsets = (uint32_t *)heic_zalloc(
                ctx, out->num_entry_point_offsets * sizeof(uint32_t));
            if (!out->entry_point_offsets) return -1;
            for (e = 0; e < out->num_entry_point_offsets; e++) {
                /* Spec stores offset_minus1; actual delta = value + 1 */
                uint32_t v = heic_bs_bits(&bs, (int)offset_len_minus1 + 1);
                out->entry_point_offsets[e] = v + 1;
            }
        }
    }

    if (pps->slice_segment_header_extension_present_flag) {
        uint32_t ext_len = heic_bs_ue(&bs); /* length in bytes */
        uint32_t b;
        for (b = 0; b < ext_len; b++)
            (void)heic_bs_bits(&bs, 8);
    }

    /* Slice header ends with byte_alignment (imazen/heic slice.rs): always
       consume alignment_bit_equal_to_one, then discard any remaining bits in the
       current byte.  When the header is already byte-aligned, the old
       "only-if-unaligned" path skipped this and CABAC started one byte early
       (nokia_grid3x2: split_cu desync, mse ~13k). */
    (void)heic_bs_bit(&bs); /* alignment_bit_equal_to_one */
    heic_bs_byte_align(&bs);

    if (bs.error) return -1;
    out->data_offset = bs.byte_pos;
    out->slice_qp_y = 26 + (int)pps->init_qp_minus26 + (int)out->slice_qp_delta;
    return 0;
}

void heic_slice_header_free(heic_ctx *ctx, heic_slice_header *sh)
{
    if (!sh) return;
    heic_free_buf(ctx, sh->entry_point_offsets);
    sh->entry_point_offsets = NULL;
    sh->num_entry_point_offsets = 0;
}
