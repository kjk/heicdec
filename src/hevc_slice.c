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

static int parse_pred_weight_table(heic_bs *bs, const heic_sps *sps,
                                   heic_slice_header *sh)
{
    uint8_t luma_flag[2][HEIC_MAX_REF_PICS] = {{0}};
    uint8_t chroma_flag[2][HEIC_MAX_REF_PICS] = {{0}};
    uint32_t denom = heic_bs_ue(bs);
    int lists = sh->slice_type == HEIC_SLICE_B ? 2 : 1;
    int list, i, c;
    if (denom > 7) return -1;
    sh->luma_log2_weight_denom = (uint8_t)denom;
    if (sps->chroma_format_idc != 0) {
        int delta = heic_bs_se(bs);
        int chroma_denom = (int)denom + delta;
        if (chroma_denom < 0) chroma_denom = 0;
        if (chroma_denom > 7) chroma_denom = 7;
        sh->chroma_log2_weight_denom = (uint8_t)chroma_denom;
    }
    for (list = 0; list < lists; list++) {
        int n = list ? sh->num_ref_idx_l1_active
                     : sh->num_ref_idx_l0_active;
        if (n < 0 || n > HEIC_MAX_REF_PICS) return -1;
        for (i = 0; i < n; i++)
            luma_flag[list][i] = (uint8_t)heic_bs_bit(bs);
        if (sps->chroma_format_idc != 0)
            for (i = 0; i < n; i++)
                chroma_flag[list][i] = (uint8_t)heic_bs_bit(bs);
        for (i = 0; i < n; i++) {
            int luma_denom = 1 << sh->luma_log2_weight_denom;
            int chroma_denom = 1 << sh->chroma_log2_weight_denom;
            sh->luma_weight[list][i] = (int16_t)luma_denom;
            if (luma_flag[list][i]) {
                int delta = heic_bs_se(bs);
                int offset;
                if (delta < -128 || delta > 127) return -1;
                sh->luma_weight[list][i] =
                    (int16_t)(luma_denom + delta);
                offset = heic_bs_se(bs);
                if (offset < INT16_MIN || offset > INT16_MAX) return -1;
                sh->luma_offset[list][i] = (int16_t)offset;
            }
            for (c = 0; c < 2; c++)
                sh->chroma_weight[list][i][c] =
                    (int16_t)chroma_denom;
            if (chroma_flag[list][i]) {
                for (c = 0; c < 2; c++) {
                    int delta = heic_bs_se(bs);
                    int offset, round, wp_offset;
                    if (delta < -128 || delta > 127) return -1;
                    sh->chroma_weight[list][i][c] =
                        (int16_t)(chroma_denom + delta);
                    offset = heic_bs_se(bs);
                    if (offset < INT16_MIN || offset > INT16_MAX) return -1;
                    round = sh->chroma_log2_weight_denom
                        ? 1 << (sh->chroma_log2_weight_denom - 1) : 0;
                    wp_offset = offset
                        - ((128 * sh->chroma_weight[list][i][c] + round)
                           >> sh->chroma_log2_weight_denom)
                        + 128;
                    if (wp_offset < -128) wp_offset = -128;
                    if (wp_offset > 127) wp_offset = 127;
                    sh->chroma_offset[list][i][c] =
                        (int16_t)wp_offset;
                }
            }
        }
    }
    sh->has_pred_weight_table = 1;
    return bs->error ? -1 : 0;
}

int heic_parse_slice_header(heic_ctx *ctx, const heic_nal *nal,
                            const heic_sps *sps, const heic_pps *pps,
                            const heic_slice_header *independent,
                            heic_slice_header *out)
{
    heic_bs bs;
    uint32_t st;
    uint32_t segment_address = 0;
    uint8_t pps_id;
    int first, dependent = 0, no_output = 0;
    int i;

    if (!nal || !sps || !pps || !out) return -1;
    memset(out, 0, sizeof(*out));
    heic_bs_init(&bs, nal->payload, nal->payload_len);

    first = heic_bs_bit(&bs);
    if (nal_is_irap(nal->type))
        no_output = heic_bs_bit(&bs);

    pps_id = (uint8_t)heic_bs_ue(&bs);
    if (pps_id != pps->pps_pic_parameter_set_id) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "slice PPS id mismatch");
        return -1;
    }

    if (!first) {
        if (pps->dependent_slice_segments_enabled_flag)
            dependent = heic_bs_bit(&bs);
        {
            int bits = ceil_log2(sps->pic_size_in_ctbs);
            segment_address = heic_bs_bits(&bs, bits);
        }
    }
    if (segment_address >= sps->pic_size_in_ctbs
        || (dependent && (!segment_address || !independent))) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   dependent ? "dependent slice has no owning slice"
                             : "slice segment address out of range");
        return -1;
    }
    if (dependent) {
        *out = *independent;
        out->entry_point_offsets = NULL;
        out->num_entry_point_offsets = 0;
        out->first_slice_segment_in_pic_flag = 0;
        out->no_output_of_prior_pics_flag = no_output;
        out->pps_id = pps_id;
        out->dependent_slice_segment_flag = 1;
        out->slice_segment_address = segment_address;
        goto finish_header;
    }
    out->first_slice_segment_in_pic_flag = first;
    out->no_output_of_prior_pics_flag = no_output;
    out->pps_id = pps_id;
    out->slice_segment_address = segment_address;
    out->slice_address = segment_address;

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
        {
            int short_term_ref_pic_set_sps_flag = heic_bs_bit(&bs);
            if (!short_term_ref_pic_set_sps_flag) {
                if (heic_parse_st_ref_pic_set(
                        &bs, sps->num_short_term_ref_pic_sets,
                        sps->num_short_term_ref_pic_sets,
                        sps->short_term_rps, &out->inline_short_term_rps) != 0) {
                    heic_error(ctx, HEIC_SEVERITY_ERROR,
                               "invalid inline short-term RPS");
                    return -1;
                }
                out->has_inline_short_term_rps = 1;
                out->short_term_ref_pic_set_idx =
                    sps->num_short_term_ref_pic_sets;
            } else if (sps->num_short_term_ref_pic_sets > 1) {
                int bits = ceil_log2(sps->num_short_term_ref_pic_sets);
                out->short_term_ref_pic_set_idx =
                    (uint8_t)heic_bs_bits(&bs, bits);
                if (out->short_term_ref_pic_set_idx
                    >= sps->num_short_term_ref_pic_sets)
                    return -1;
            }
        }
        if (sps->long_term_ref_pics_present_flag) {
            uint32_t num_lt_sps = 0;
            uint32_t num_lt_pics;
            uint32_t total;
            int lt_poc_bits = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
            if (sps->num_long_term_ref_pics_sps > 0)
                num_lt_sps = heic_bs_ue(&bs);
            num_lt_pics = heic_bs_ue(&bs);
            if (num_lt_sps > sps->num_long_term_ref_pics_sps
                || num_lt_sps > HEIC_MAX_REF_PICS
                || num_lt_pics > HEIC_MAX_REF_PICS
                || num_lt_sps + num_lt_pics > HEIC_MAX_REF_PICS) {
                heic_error(ctx, HEIC_SEVERITY_ERROR,
                           "long-term RPS exceeds reference-picture limit");
                return -1;
            }
            out->num_long_term_sps = (uint8_t)num_lt_sps;
            out->num_long_term_pics = (uint8_t)num_lt_pics;
            total = num_lt_sps + num_lt_pics;
            for (i = 0; i < (int)total; i++) {
                if ((uint32_t)i < num_lt_sps) {
                    uint32_t idx = 0;
                    if (sps->num_long_term_ref_pics_sps > 1)
                        idx = heic_bs_bits(
                            &bs, ceil_log2(sps->num_long_term_ref_pics_sps));
                    if (idx >= sps->num_long_term_ref_pics_sps)
                        return -1;
                    out->lt_idx_sps[i] = (uint8_t)idx;
                    out->poc_lsb_lt[i] =
                        sps->lt_ref_pic_poc_lsb_sps[idx];
                    out->used_by_curr_pic_lt_flag[i] =
                        sps->used_by_curr_pic_lt_sps_flag[idx];
                } else {
                    out->poc_lsb_lt[i] = heic_bs_bits(&bs, lt_poc_bits);
                    out->used_by_curr_pic_lt_flag[i] =
                        (uint8_t)heic_bs_bit(&bs);
                }
                out->delta_poc_msb_present_flag[i] =
                    (uint8_t)heic_bs_bit(&bs);
                if (out->delta_poc_msb_present_flag[i]) {
                    uint32_t cycle = heic_bs_ue(&bs);
                    if (i != 0 && i != (int)num_lt_sps) {
                        if (cycle > UINT32_MAX -
                                      out->delta_poc_msb_cycle_lt[i - 1])
                            return -1;
                        cycle += out->delta_poc_msb_cycle_lt[i - 1];
                    }
                    out->delta_poc_msb_cycle_lt[i] = cycle;
                }
            }
        }
        if (sps->sps_temporal_mvp_enabled_flag)
            out->slice_temporal_mvp_enabled_flag = heic_bs_bit(&bs);
    }

    if (sps->sample_adaptive_offset_enabled_flag) {
        out->slice_sao_luma_flag = heic_bs_bit(&bs);
        if (sps->chroma_format_idc != 0)
            out->slice_sao_chroma_flag = heic_bs_bit(&bs);
    }

    if (pps->num_ref_idx_l0_default_active_minus1 > 14
        || pps->num_ref_idx_l1_default_active_minus1 > 14) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "PPS default active reference count out of range");
        return -1;
    }
    out->num_ref_idx_l0_active =
        (uint8_t)(pps->num_ref_idx_l0_default_active_minus1 + 1);
    out->num_ref_idx_l1_active =
        out->slice_type == HEIC_SLICE_B
            ? (uint8_t)(pps->num_ref_idx_l1_default_active_minus1 + 1)
            : 0;
    out->collocated_from_l0_flag = 1;
    out->max_num_merge_cand = 5;

    if (out->slice_type != HEIC_SLICE_I) {
        int override = heic_bs_bit(&bs);
        if (override) {
            uint32_t n = heic_bs_ue(&bs);
            if (n > 14) return -1;
            out->num_ref_idx_l0_active = (uint8_t)(n + 1);
            if (out->slice_type == HEIC_SLICE_B) {
                n = heic_bs_ue(&bs);
                if (n > 14) return -1;
                out->num_ref_idx_l1_active = (uint8_t)(n + 1);
            }
        }
        if (out->num_ref_idx_l0_active > HEIC_MAX_REF_PICS
            || out->num_ref_idx_l1_active > HEIC_MAX_REF_PICS) {
            heic_error(ctx, HEIC_SEVERITY_ERROR,
                       "active reference count exceeds limit");
            return -1;
        }
        if (pps->lists_modification_present_flag) {
            const heic_st_rps *rps = out->has_inline_short_term_rps
                ? &out->inline_short_term_rps
                : &sps->short_term_rps[out->short_term_ref_pic_set_idx];
            int used = 0;
            for (i = 0; i < rps->num_negative_pics; i++)
                used += rps->used_by_curr_pic_s0[i] != 0;
            for (i = 0; i < rps->num_positive_pics; i++)
                used += rps->used_by_curr_pic_s1[i] != 0;
            for (i = 0; i < out->num_long_term_sps + out->num_long_term_pics; i++)
                used += out->used_by_curr_pic_lt_flag[i] != 0;
            if (used > 1) {
                int bits = ceil_log2((uint32_t)used);
                out->ref_pic_list_modification_flag_l0 = heic_bs_bit(&bs);
                if (out->ref_pic_list_modification_flag_l0) {
                    for (i = 0; i < out->num_ref_idx_l0_active; i++) {
                        uint32_t entry = heic_bs_bits(&bs, bits);
                        if (entry >= (uint32_t)used) return -1;
                        out->list_entry_l0[i] = (uint8_t)entry;
                    }
                }
                if (out->slice_type == HEIC_SLICE_B) {
                    out->ref_pic_list_modification_flag_l1 =
                        heic_bs_bit(&bs);
                    if (out->ref_pic_list_modification_flag_l1) {
                        for (i = 0; i < out->num_ref_idx_l1_active; i++) {
                            uint32_t entry = heic_bs_bits(&bs, bits);
                            if (entry >= (uint32_t)used) return -1;
                            out->list_entry_l1[i] = (uint8_t)entry;
                        }
                    }
                }
            }
        }
        if (out->slice_type == HEIC_SLICE_B)
            out->mvd_l1_zero_flag = heic_bs_bit(&bs);
        if (pps->cabac_init_present_flag)
            out->cabac_init_flag = heic_bs_bit(&bs);
        if (out->slice_temporal_mvp_enabled_flag) {
            int n_col;
            if (out->slice_type == HEIC_SLICE_B)
                out->collocated_from_l0_flag = heic_bs_bit(&bs);
            n_col = out->collocated_from_l0_flag
                ? out->num_ref_idx_l0_active
                : out->num_ref_idx_l1_active;
            if (n_col > 1) {
                uint32_t idx = heic_bs_ue(&bs);
                if (idx >= (uint32_t)n_col) return -1;
                out->collocated_ref_idx = (uint8_t)idx;
            }
        }
        if ((out->slice_type == HEIC_SLICE_P && pps->weighted_pred_flag)
            || (out->slice_type == HEIC_SLICE_B
                && pps->weighted_bipred_flag)) {
            if (parse_pred_weight_table(&bs, sps, out) != 0) {
                heic_error(ctx, HEIC_SEVERITY_ERROR,
                           "invalid weighted prediction table");
                return -1;
            }
        }
        {
            uint32_t five_minus = heic_bs_ue(&bs);
            if (five_minus > 4) return -1;
            out->max_num_merge_cand = (uint8_t)(5 - five_minus);
        }
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

finish_header:
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
