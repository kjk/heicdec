/* hevc_params.c -- SPS/PPS parse (subset needed for still HEIC; expand over time) */
#include "heic_internal.h"

/* Skip profile_tier_level (H.265 7.3.3) for max_sub_layers_minus1. */
static void skip_profile_tier_level(heic_bs *bs, int max_sub_layers_minus1)
{
    int i, j;
    (void)heic_bs_bits(bs, 2 + 1 + 5); /* space, tier, profile_idc */
    (void)heic_bs_bits(bs, 32);        /* compatibility flags */
    (void)heic_bs_bits(bs, 48);        /* constraint indicator (6 bytes) */
    (void)heic_bs_bits(bs, 8);         /* level_idc */
    {
        int sub_layer_profile_present[8];
        int sub_layer_level_present[8];
        for (i = 0; i < max_sub_layers_minus1; i++) {
            sub_layer_profile_present[i] = heic_bs_bit(bs);
            sub_layer_level_present[i] = heic_bs_bit(bs);
        }
        if (max_sub_layers_minus1 > 0)
            for (i = max_sub_layers_minus1; i < 8; i++) (void)heic_bs_bits(bs, 2);
        for (i = 0; i < max_sub_layers_minus1; i++) {
            if (sub_layer_profile_present[i]) {
                (void)heic_bs_bits(bs, 2 + 1 + 5);
                (void)heic_bs_bits(bs, 32);
                (void)heic_bs_bits(bs, 48);
            }
            if (sub_layer_level_present[i]) (void)heic_bs_bits(bs, 8);
        }
    }
    (void)j;
}

static const uint8_t HEIC_DEFAULT_INTRA_8X8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
    17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
    24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
    29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115
};

static const uint8_t HEIC_DEFAULT_INTER_8X8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
    18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
    28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

static void scaling_list_default(heic_scaling_list *out)
{
    int size_id, matrix_id;
    memset(out, 16, sizeof(*out));
    for (size_id = 1; size_id < 4; size_id++) {
        for (matrix_id = 0; matrix_id < 3; matrix_id++)
            memcpy(out->coef[size_id][matrix_id], HEIC_DEFAULT_INTRA_8X8, 64);
        for (matrix_id = 3; matrix_id < 6; matrix_id++)
            memcpy(out->coef[size_id][matrix_id], HEIC_DEFAULT_INTER_8X8, 64);
    }
}

static int parse_scaling_list_data(heic_bs *bs, heic_scaling_list *out)
{
    int size_id, matrix_id;
    scaling_list_default(out);
    for (size_id = 0; size_id < 4; size_id++) {
        int matrix_step = size_id == 3 ? 3 : 1;
        for (matrix_id = 0; matrix_id < 6; matrix_id += matrix_step) {
            if (!heic_bs_bit(bs)) {
                uint32_t delta = heic_bs_ue(bs);
                uint32_t scaled_delta = delta * (uint32_t)matrix_step;
                if (scaled_delta > (uint32_t)matrix_id) {
                    bs->error = 1;
                    return -1;
                }
                if (delta != 0) {
                    int ref_id = matrix_id - (int)scaled_delta;
                    memcpy(out->coef[size_id][matrix_id],
                           out->coef[size_id][ref_id], 64);
                    if (size_id >= 2)
                        out->dc_coef[size_id - 2][matrix_id] =
                            out->dc_coef[size_id - 2][ref_id];
                }
            } else {
                int coef_num = HEIC_MIN(64, 1 << (4 + (size_id << 1)));
                int next_coef = 8;
                int i;
                if (size_id > 1) {
                    int32_t dc_minus8 = heic_bs_se(bs);
                    if (dc_minus8 < -7 || dc_minus8 > 247) {
                        bs->error = 1;
                        return -1;
                    }
                    next_coef = (int)dc_minus8 + 8;
                    out->dc_coef[size_id - 2][matrix_id] = (uint8_t)next_coef;
                }
                for (i = 0; i < coef_num; i++) {
                    int32_t delta = heic_bs_se(bs);
                    if (delta < -128 || delta > 127) {
                        bs->error = 1;
                        return -1;
                    }
                    next_coef = (next_coef + (int)delta + 256) & 255;
                    out->coef[size_id][matrix_id][i] = (uint8_t)next_coef;
                }
            }
        }
    }
    return bs->error ? -1 : 0;
}

static void sort_rps(int32_t *delta, uint8_t *used, int n, int increasing)
{
    int i;
    for (i = 1; i < n; i++) {
        int32_t d = delta[i];
        uint8_t u = used[i];
        int j = i;
        while (j > 0 && (increasing ? delta[j - 1] > d : delta[j - 1] < d)) {
            delta[j] = delta[j - 1];
            used[j] = used[j - 1];
            j--;
        }
        delta[j] = d;
        used[j] = u;
    }
}

int heic_parse_st_ref_pic_set(heic_bs *bs, int idx, int num_sets,
                              const heic_st_rps *sets, heic_st_rps *out)
{
    int inter = idx != 0 ? heic_bs_bit(bs) : 0;
    memset(out, 0, sizeof(*out));
    if (!inter) {
        uint32_t num_neg = heic_bs_ue(bs);
        uint32_t num_pos = heic_bs_ue(bs);
        uint32_t i;
        int32_t prev = 0;
        if (num_neg > HEIC_MAX_REF_PICS || num_pos > HEIC_MAX_REF_PICS ||
            num_neg + num_pos > HEIC_MAX_REF_PICS) {
            bs->error = 1;
            return -1;
        }
        out->num_negative_pics = (uint8_t)num_neg;
        out->num_positive_pics = (uint8_t)num_pos;
        for (i = 0; i < num_neg; i++) {
            uint32_t minus1 = heic_bs_ue(bs);
            prev -= (int32_t)minus1 + 1;
            out->delta_poc_s0[i] = prev;
            out->used_by_curr_pic_s0[i] = (uint8_t)heic_bs_bit(bs);
        }
        prev = 0;
        for (i = 0; i < num_pos; i++) {
            uint32_t minus1 = heic_bs_ue(bs);
            prev += (int32_t)minus1 + 1;
            out->delta_poc_s1[i] = prev;
            out->used_by_curr_pic_s1[i] = (uint8_t)heic_bs_bit(bs);
        }
        return bs->error ? -1 : 0;
    }

    {
        uint32_t delta_idx_minus1 = idx == num_sets ? heic_bs_ue(bs) : 0;
        int ref_idx = idx - (int)delta_idx_minus1 - 1;
        int sign, delta_rps;
        uint32_t abs_minus1;
        const heic_st_rps *ref;
        int ref_count, j, neg = 0, pos = 0;
        int32_t ref_delta[HEIC_MAX_REF_PICS];
        uint8_t used[HEIC_MAX_REF_PICS + 1];
        uint8_t use_delta[HEIC_MAX_REF_PICS + 1];

        if (ref_idx < 0 || ref_idx >= idx) {
            bs->error = 1;
            return -1;
        }
        sign = heic_bs_bit(bs);
        abs_minus1 = heic_bs_ue(bs);
        if (abs_minus1 > INT32_MAX - 1u) {
            bs->error = 1;
            return -1;
        }
        delta_rps = (sign ? -1 : 1) * ((int)abs_minus1 + 1);
        ref = &sets[ref_idx];
        ref_count = ref->num_negative_pics + ref->num_positive_pics;
        if (ref_count > HEIC_MAX_REF_PICS) {
            bs->error = 1;
            return -1;
        }
        for (j = 0; j < ref->num_negative_pics; j++)
            ref_delta[j] = ref->delta_poc_s0[j];
        for (j = 0; j < ref->num_positive_pics; j++)
            ref_delta[ref->num_negative_pics + j] = ref->delta_poc_s1[j];
        memset(used, 0, sizeof(used));
        memset(use_delta, 1, sizeof(use_delta));
        for (j = 0; j <= ref_count; j++) {
            used[j] = (uint8_t)heic_bs_bit(bs);
            if (!used[j]) use_delta[j] = (uint8_t)heic_bs_bit(bs);
        }
        if ((used[ref_count] || use_delta[ref_count]) && delta_rps != 0) {
            if (delta_rps < 0) {
                out->delta_poc_s0[neg] = delta_rps;
                out->used_by_curr_pic_s0[neg++] = used[ref_count];
            } else {
                out->delta_poc_s1[pos] = delta_rps;
                out->used_by_curr_pic_s1[pos++] = used[ref_count];
            }
        }
        for (j = 0; j < ref_count; j++) {
            int32_t d;
            if (!used[j] && !use_delta[j]) continue;
            d = ref_delta[j] + delta_rps;
            if (d < 0 && neg < HEIC_MAX_REF_PICS) {
                out->delta_poc_s0[neg] = d;
                out->used_by_curr_pic_s0[neg++] = used[j];
            } else if (d > 0 && pos < HEIC_MAX_REF_PICS) {
                out->delta_poc_s1[pos] = d;
                out->used_by_curr_pic_s1[pos++] = used[j];
            }
        }
        if (neg + pos > HEIC_MAX_REF_PICS) {
            bs->error = 1;
            return -1;
        }
        out->num_negative_pics = (uint8_t)neg;
        out->num_positive_pics = (uint8_t)pos;
        sort_rps(out->delta_poc_s0, out->used_by_curr_pic_s0, neg, 0);
        sort_rps(out->delta_poc_s1, out->used_by_curr_pic_s1, pos, 1);
    }
    return bs->error ? -1 : 0;
}

int heic_parse_sps(heic_ctx *ctx, const uint8_t *rbsp, size_t len, heic_sps *out)
{
    heic_bs bs;
    uint32_t min_cb, ctb, min_tb, max_tb;
    uint32_t num_st_rps, i;

    memset(out, 0, sizeof(*out));
    scaling_list_default(&out->scaling_list);
    if (!rbsp || len < 4) return -1;
    heic_bs_init(&bs, rbsp, len);

    out->sps_video_parameter_set_id = (uint8_t)heic_bs_bits(&bs, 4);
    out->sps_max_sub_layers_minus1 = (uint8_t)heic_bs_bits(&bs, 3);
    out->sps_temporal_id_nesting_flag = heic_bs_bit(&bs);
    skip_profile_tier_level(&bs, out->sps_max_sub_layers_minus1);

    (void)heic_bs_ue(&bs); /* sps_seq_parameter_set_id */
    out->chroma_format_idc = (uint8_t)heic_bs_ue(&bs);
    if (out->chroma_format_idc == 3) out->separate_colour_plane_flag = heic_bs_bit(&bs);

    out->pic_width_in_luma_samples = heic_bs_ue(&bs);
    out->pic_height_in_luma_samples = heic_bs_ue(&bs);
    if (out->pic_width_in_luma_samples == 0 || out->pic_height_in_luma_samples == 0 ||
        out->pic_width_in_luma_samples > 16384 || out->pic_height_in_luma_samples > 16384) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "SPS invalid dimensions");
        return -1;
    }

    out->conformance_window_flag = heic_bs_bit(&bs);
    if (out->conformance_window_flag) {
        out->conf_win_left_offset = heic_bs_ue(&bs);
        out->conf_win_right_offset = heic_bs_ue(&bs);
        out->conf_win_top_offset = heic_bs_ue(&bs);
        out->conf_win_bottom_offset = heic_bs_ue(&bs);
    }

    out->bit_depth_luma_minus8 = (uint8_t)heic_bs_ue(&bs);
    out->bit_depth_chroma_minus8 = (uint8_t)heic_bs_ue(&bs);
    if (out->bit_depth_luma_minus8 > 8 || out->bit_depth_chroma_minus8 > 8) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "SPS bit_depth_minus8 out of range");
        return -1;
    }
    out->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)heic_bs_ue(&bs);
    if (out->log2_max_pic_order_cnt_lsb_minus4 > 12) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "SPS log2_max_poc_lsb_minus4 out of range");
        return -1;
    }

    {
        int sublayer_ordering_info = heic_bs_bit(&bs);
        int start = sublayer_ordering_info ? 0 : out->sps_max_sub_layers_minus1;
        for (i = (uint32_t)start; i <= out->sps_max_sub_layers_minus1; i++) {
            (void)heic_bs_ue(&bs);
            (void)heic_bs_ue(&bs);
            (void)heic_bs_ue(&bs);
        }
    }

    out->log2_min_luma_coding_block_size_minus3 = (uint8_t)heic_bs_ue(&bs);
    out->log2_diff_max_min_luma_coding_block_size = (uint8_t)heic_bs_ue(&bs);
    out->log2_min_luma_transform_block_size_minus2 = (uint8_t)heic_bs_ue(&bs);
    out->log2_diff_max_min_luma_transform_block_size = (uint8_t)heic_bs_ue(&bs);
    /* Spec ranges: min_cb 8..64, ctb ≤ 64, min_tb 4..32 (H.265 7.4.3.2). */
    if (out->log2_min_luma_coding_block_size_minus3 > 3
        || out->log2_diff_max_min_luma_coding_block_size > 3
        || out->log2_min_luma_transform_block_size_minus2 > 3
        || out->log2_diff_max_min_luma_transform_block_size > 3) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "SPS coding/transform size fields out of range");
        return -1;
    }
    out->max_transform_hierarchy_depth_inter = (uint8_t)heic_bs_ue(&bs);
    out->max_transform_hierarchy_depth_intra = (uint8_t)heic_bs_ue(&bs);
    if (out->max_transform_hierarchy_depth_inter > 5
        || out->max_transform_hierarchy_depth_intra > 5) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "SPS max_transform_hierarchy_depth out of range");
        return -1;
    }

    out->scaling_list_enabled_flag = heic_bs_bit(&bs);
    if (out->scaling_list_enabled_flag) {
        out->sps_scaling_list_data_present_flag = heic_bs_bit(&bs);
        if (out->sps_scaling_list_data_present_flag
            && parse_scaling_list_data(&bs, &out->scaling_list) != 0)
            return -1;
    }

    out->amp_enabled_flag = heic_bs_bit(&bs);
    out->sample_adaptive_offset_enabled_flag = heic_bs_bit(&bs);
    out->pcm_enabled_flag = heic_bs_bit(&bs);
    if (out->pcm_enabled_flag) {
        out->pcm_sample_bit_depth_luma_minus1 = (uint8_t)heic_bs_bits(&bs, 4);
        out->pcm_sample_bit_depth_chroma_minus1 = (uint8_t)heic_bs_bits(&bs, 4);
        out->log2_min_pcm_luma_coding_block_size_minus3 = (uint8_t)heic_bs_ue(&bs);
        out->log2_diff_max_min_pcm_luma_coding_block_size = (uint8_t)heic_bs_ue(&bs);
        out->pcm_loop_filter_disabled_flag = heic_bs_bit(&bs);
    }

    num_st_rps = heic_bs_ue(&bs);
    if (num_st_rps > 64) return -1;
    out->num_short_term_ref_pic_sets = (uint8_t)num_st_rps;
    for (i = 0; i < num_st_rps; i++)
        if (heic_parse_st_ref_pic_set(&bs, (int)i, (int)num_st_rps,
                                      out->short_term_rps,
                                      &out->short_term_rps[i]) != 0)
            return -1;

    out->long_term_ref_pics_present_flag = heic_bs_bit(&bs);
    if (out->long_term_ref_pics_present_flag) {
        uint32_t num_lt = heic_bs_ue(&bs);
        uint32_t k;
        /* Spec: num_long_term_ref_pics_sps in 0..32. */
        if (num_lt > 32) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "SPS num_long_term_ref_pics_sps out of range");
            return -1;
        }
        out->num_long_term_ref_pics_sps = (uint8_t)num_lt;
        for (k = 0; k < num_lt; k++) {
            out->lt_ref_pic_poc_lsb_sps[k] =
                heic_bs_bits(&bs, out->log2_max_pic_order_cnt_lsb_minus4 + 4);
            out->used_by_curr_pic_lt_sps_flag[k] = (uint8_t)heic_bs_bit(&bs);
        }
    }
    out->sps_temporal_mvp_enabled_flag = heic_bs_bit(&bs);
    out->strong_intra_smoothing_enabled_flag = heic_bs_bit(&bs);

    out->vui_parameters_present_flag = heic_bs_bit(&bs);
    if (out->vui_parameters_present_flag) {
        /* Minimal VUI: we only need colour description when present. */
        if (heic_bs_bit(&bs)) { /* aspect_ratio_info */
            uint8_t idc = (uint8_t)heic_bs_bits(&bs, 8);
            if (idc == 255) {
                (void)heic_bs_bits(&bs, 16);
                (void)heic_bs_bits(&bs, 16);
            }
        }
        if (heic_bs_bit(&bs)) (void)heic_bs_bit(&bs); /* overscan */
        out->video_signal_type_present_flag = heic_bs_bit(&bs);
        if (out->video_signal_type_present_flag) {
            (void)heic_bs_bits(&bs, 3); /* video_format */
            out->video_full_range_flag = heic_bs_bit(&bs);
            out->colour_description_present_flag = heic_bs_bit(&bs);
            if (out->colour_description_present_flag) {
                out->colour_primaries = (uint8_t)heic_bs_bits(&bs, 8);
                out->transfer_characteristics = (uint8_t)heic_bs_bits(&bs, 8);
                out->matrix_coeffs = (uint8_t)heic_bs_bits(&bs, 8);
            }
        }
        /* remaining VUI ignored for still-image probe/decode bootstrap */
    }

    if (bs.error) return -1;

    /* derived sizes */
    out->log2_min_cb_size = (uint8_t)(out->log2_min_luma_coding_block_size_minus3 + 3);
    out->log2_ctb_size =
        (uint8_t)(out->log2_min_cb_size + out->log2_diff_max_min_luma_coding_block_size);
    out->log2_min_tb_size = (uint8_t)(out->log2_min_luma_transform_block_size_minus2 + 2);
    out->log2_max_tb_size =
        (uint8_t)(out->log2_min_tb_size + out->log2_diff_max_min_luma_transform_block_size);
    /* H.265: MaxTbSizeY ≤ min(CtbSizeY, 32); MinTbSizeY ≤ MinCbSizeY. */
    if (out->log2_max_tb_size > 5 || out->log2_min_tb_size > out->log2_min_cb_size
        || out->log2_max_tb_size > out->log2_ctb_size || out->log2_ctb_size > 6) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "SPS derived coding/transform sizes invalid");
        return -1;
    }

    min_cb = 1u << out->log2_min_cb_size;
    ctb = 1u << out->log2_ctb_size;
    min_tb = 1u << out->log2_min_tb_size;
    max_tb = 1u << out->log2_max_tb_size;
    (void)min_tb;
    (void)max_tb;
    /* pic_width/height_in_luma_samples shall be integer multiples of MinCbSizeY. */
    if ((out->pic_width_in_luma_samples % min_cb) != 0
        || (out->pic_height_in_luma_samples % min_cb) != 0) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "SPS dimensions not multiple of MinCbSizeY");
        return -1;
    }
    out->ctb_width = ctb;
    out->ctb_height = ctb;
    out->pic_width_in_ctbs = (out->pic_width_in_luma_samples + ctb - 1) / ctb;
    out->pic_height_in_ctbs = (out->pic_height_in_luma_samples + ctb - 1) / ctb;
    out->pic_size_in_ctbs = out->pic_width_in_ctbs * out->pic_height_in_ctbs;
    return 0;
}

int heic_parse_pps(heic_ctx *ctx, const uint8_t *rbsp, size_t len, heic_pps *out)
{
    heic_bs bs;
    memset(out, 0, sizeof(*out));
    scaling_list_default(&out->scaling_list);
    if (!rbsp || len < 1) return -1;
    heic_bs_init(&bs, rbsp, len);

    out->pps_pic_parameter_set_id = (uint8_t)heic_bs_ue(&bs);
    out->pps_seq_parameter_set_id = (uint8_t)heic_bs_ue(&bs);
    out->dependent_slice_segments_enabled_flag = heic_bs_bit(&bs);
    out->output_flag_present_flag = heic_bs_bit(&bs);
    out->num_extra_slice_header_bits = (uint8_t)heic_bs_bits(&bs, 3);
    out->sign_data_hiding_enabled_flag = heic_bs_bit(&bs);
    out->cabac_init_present_flag = heic_bs_bit(&bs);
    out->num_ref_idx_l0_default_active_minus1 = (uint8_t)heic_bs_ue(&bs);
    out->num_ref_idx_l1_default_active_minus1 = (uint8_t)heic_bs_ue(&bs);
    out->init_qp_minus26 = (int8_t)heic_bs_se(&bs);
    out->constrained_intra_pred_flag = heic_bs_bit(&bs);
    out->transform_skip_enabled_flag = heic_bs_bit(&bs);
    out->cu_qp_delta_enabled_flag = heic_bs_bit(&bs);
    if (out->cu_qp_delta_enabled_flag) {
        uint32_t d = heic_bs_ue(&bs);
        /* Must not exceed log2_ctb_size (≤6); unbounded UE underflows qg math. */
        if (d > 6) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "PPS diff_cu_qp_delta_depth out of range");
            return -1;
        }
        out->diff_cu_qp_delta_depth = (uint8_t)d;
    }
    out->pps_cb_qp_offset = (int8_t)heic_bs_se(&bs);
    out->pps_cr_qp_offset = (int8_t)heic_bs_se(&bs);
    out->pps_slice_chroma_qp_offsets_present_flag = heic_bs_bit(&bs);
    out->weighted_pred_flag = heic_bs_bit(&bs);
    out->weighted_bipred_flag = heic_bs_bit(&bs);
    out->transquant_bypass_enabled_flag = heic_bs_bit(&bs);
    out->tiles_enabled_flag = heic_bs_bit(&bs);
    out->entropy_coding_sync_enabled_flag = heic_bs_bit(&bs);
    if (out->tiles_enabled_flag) {
        uint32_t nc = heic_bs_ue(&bs);
        uint32_t nr = heic_bs_ue(&bs);
        /* H.265 Table A.1: at most 20 columns / 22 rows. Fixed col_bd[65] /
         * row_bd[65] on the CTU stack also require this. */
        if (nc > 19 || nr > 21) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "PPS tile grid exceeds HEVC max 20x22");
            return -1;
        }
        out->num_tile_columns_minus1 = (uint16_t)nc;
        out->num_tile_rows_minus1 = (uint16_t)nr;
        out->uniform_spacing_flag = heic_bs_bit(&bs);
        if (!out->uniform_spacing_flag) {
            uint16_t i;
            out->column_width_minus1 =
                (uint16_t *)heic_zalloc(ctx, (size_t)nc * sizeof(uint16_t));
            out->row_height_minus1 =
                (uint16_t *)heic_zalloc(ctx, (size_t)nr * sizeof(uint16_t));
            if ((!out->column_width_minus1 && nc) || (!out->row_height_minus1 && nr))
                return -1;
            for (i = 0; i < out->num_tile_columns_minus1; i++)
                out->column_width_minus1[i] = (uint16_t)heic_bs_ue(&bs);
            for (i = 0; i < out->num_tile_rows_minus1; i++)
                out->row_height_minus1[i] = (uint16_t)heic_bs_ue(&bs);
        }
        out->loop_filter_across_tiles_enabled_flag = heic_bs_bit(&bs);
    }
    out->pps_loop_filter_across_slices_enabled_flag = heic_bs_bit(&bs);
    out->deblocking_filter_control_present_flag = heic_bs_bit(&bs);
    if (out->deblocking_filter_control_present_flag) {
        out->deblocking_filter_override_enabled_flag = heic_bs_bit(&bs);
        out->pps_deblocking_filter_disabled_flag = heic_bs_bit(&bs);
        if (!out->pps_deblocking_filter_disabled_flag) {
            out->pps_beta_offset_div2 = (int8_t)heic_bs_se(&bs);
            out->pps_tc_offset_div2 = (int8_t)heic_bs_se(&bs);
        }
    }
    out->pps_scaling_list_data_present_flag = heic_bs_bit(&bs);
    if (out->pps_scaling_list_data_present_flag)
        if (parse_scaling_list_data(&bs, &out->scaling_list) != 0)
            return -1;
    out->lists_modification_present_flag = heic_bs_bit(&bs);
    out->log2_parallel_merge_level_minus2 = (uint8_t)heic_bs_ue(&bs);
    out->slice_segment_header_extension_present_flag = heic_bs_bit(&bs);
    /* pps_extension_* ignored for stills; stop here (bitstream may have more). */
    if (bs.error) return -1;
    return 0;
}

void heic_pps_free(heic_ctx *ctx, heic_pps *pps)
{
    if (!pps) return;
    heic_free_buf(ctx, pps->column_width_minus1);
    heic_free_buf(ctx, pps->row_height_minus1);
    memset(pps, 0, sizeof(*pps));
}
