/* hevc_decode.c -- HEVC still-image entry */
#include "heic_internal.h"

static int find_ref_by_poc(const heic_frame *const *refs, int n_refs,
                           int poc, uint32_t poc_mask, int lsb_only)
{
    int i;
    for (i = 0; i < n_refs; i++) {
        if (!refs[i] || !refs[i]->poc_valid) continue;
        if ((!lsb_only && refs[i]->poc == poc)
            || (lsb_only
                && (((uint32_t)refs[i]->poc & poc_mask)
                    == ((uint32_t)poc & poc_mask))))
            return i;
    }
    return -1;
}

static int build_ref_lists(heic_ctx *ctx, const heic_sps *sps,
                           const heic_slice_header *sh,
                           int curr_poc,
                           const heic_frame *const *refs, int n_refs,
                           const heic_frame **l0, int *n_l0,
                           const heic_frame **l1, int *n_l1)
{
    const heic_st_rps *rps = sh->has_inline_short_term_rps
        ? &sh->inline_short_term_rps
        : &sps->short_term_rps[sh->short_term_ref_pic_set_idx];
    const heic_frame *before[HEIC_MAX_REF_PICS];
    const heic_frame *after[HEIC_MAX_REF_PICS];
    const heic_frame *lt[HEIC_MAX_REF_PICS];
    const heic_frame *temp0[HEIC_MAX_REF_PICS];
    const heic_frame *temp1[HEIC_MAX_REF_PICS];
    uint32_t poc_mask =
        (1u << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1u;
    int n_before = 0, n_after = 0, n_lt = 0, n_temp = 0;
    int i;

    /* StCurrBefore, StCurrAfter, then LtCurr (H.265 8.3.4). */
    for (i = 0; i < rps->num_negative_pics; i++) {
        int idx;
        if (!rps->used_by_curr_pic_s0[i]) continue;
        idx = find_ref_by_poc(refs, n_refs,
                              curr_poc + rps->delta_poc_s0[i],
                              poc_mask, 0);
        if (idx >= 0 && n_before < HEIC_MAX_REF_PICS)
            before[n_before++] = refs[idx];
    }
    for (i = 0; i < rps->num_positive_pics; i++) {
        int idx;
        if (!rps->used_by_curr_pic_s1[i]) continue;
        idx = find_ref_by_poc(refs, n_refs,
                              curr_poc + rps->delta_poc_s1[i],
                              poc_mask, 0);
        if (idx >= 0 && n_after < HEIC_MAX_REF_PICS)
            after[n_after++] = refs[idx];
    }
    for (i = 0; i < sh->num_long_term_sps + sh->num_long_term_pics; i++) {
        int idx;
        if (!sh->used_by_curr_pic_lt_flag[i]) continue;
        idx = find_ref_by_poc(refs, n_refs, (int)sh->poc_lsb_lt[i],
                              poc_mask, 1);
        if (idx >= 0 && n_lt < HEIC_MAX_REF_PICS)
            lt[n_lt++] = refs[idx];
    }

    /* Some HEIF writers leave POC metadata ambiguous across independent items.
       Preserve iref order as a bounded fallback when no POC match was possible. */
    n_temp = n_before + n_after + n_lt;
    if (n_temp == 0) {
        for (i = 0; i < n_refs && n_before < HEIC_MAX_REF_PICS; i++)
            if (refs[i]) before[n_before++] = refs[i];
        n_temp = n_before;
    }
    if (n_temp == 0) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "no supplied HEVC reference matches active RPS");
        return -1;
    }
    n_temp = 0;
    for (i = 0; i < n_before && n_temp < HEIC_MAX_REF_PICS; i++)
        temp0[n_temp++] = before[i];
    for (i = 0; i < n_after && n_temp < HEIC_MAX_REF_PICS; i++)
        temp0[n_temp++] = after[i];
    for (i = 0; i < n_lt && n_temp < HEIC_MAX_REF_PICS; i++)
        temp0[n_temp++] = lt[i];
    for (i = 0; i < sh->num_ref_idx_l0_active; i++) {
        int entry = sh->ref_pic_list_modification_flag_l0
            ? sh->list_entry_l0[i]
            : i % n_temp;
        if (entry < 0 || entry >= n_temp) {
            heic_error(ctx, HEIC_SEVERITY_ERROR,
                       "HEVC L0 list entry has no supplied reference");
            return -1;
        }
        l0[i] = temp0[entry];
    }
    *n_l0 = sh->num_ref_idx_l0_active;

    *n_l1 = 0;
    if (sh->slice_type == HEIC_SLICE_B) {
        n_temp = 0;
        for (i = 0; i < n_after && n_temp < HEIC_MAX_REF_PICS; i++)
            temp1[n_temp++] = after[i];
        for (i = 0; i < n_before && n_temp < HEIC_MAX_REF_PICS; i++)
            temp1[n_temp++] = before[i];
        for (i = 0; i < n_lt && n_temp < HEIC_MAX_REF_PICS; i++)
            temp1[n_temp++] = lt[i];
        for (i = 0; i < sh->num_ref_idx_l1_active; i++) {
            int entry = sh->ref_pic_list_modification_flag_l1
                ? sh->list_entry_l1[i]
                : i % n_temp;
            if (entry < 0 || entry >= n_temp) {
                heic_error(ctx, HEIC_SEVERITY_ERROR,
                           "HEVC L1 list entry has no supplied reference");
                return -1;
            }
            l1[i] = temp1[entry];
        }
        *n_l1 = sh->num_ref_idx_l1_active;
    }
    return 0;
}

static int heic_hevc_decode_impl(heic_ctx *ctx, const heic_hvcc *cfg,
                                 const uint8_t *data, size_t len,
                                 const heic_frame *const *refs, int n_refs,
                                 heic_frame *out, const heic_abort *ab)
{
    heic_nal *nals = NULL;
    int n_nals = 0, i;
    heic_sps sps;
    heic_pps pps;
    int have_sps = 0, have_pps = 0;
    int length_size;
    heic_nal param;
    int sub_w = 2, sub_h = 2;
    int decode_ok = 0;

    memset(out, 0, sizeof(*out));
    memset(&sps, 0, sizeof(sps));
    memset(&pps, 0, sizeof(pps));
    if (!ctx || !cfg || !data) return -1;
    if (heic_abort_check(ab)) return -1;

    /* Parameter sets from hvcC */
    for (i = 0; i < cfg->n_nal_units; i++) {
        if (heic_parse_single_nal(ctx, cfg->nal_units[i], cfg->nal_unit_lens[i], &param) != 0)
            continue;
        if (param.type == HEIC_NAL_SPS) {
            if (heic_parse_sps(ctx, param.payload, param.payload_len, &sps) == 0)
                have_sps = 1;
        } else if (param.type == HEIC_NAL_PPS) {
            heic_pps_free(ctx, &pps);
            if (heic_parse_pps(ctx, param.payload, param.payload_len, &pps) == 0)
                have_pps = 1;
        }
        heic_nal_free(ctx, &param);
    }
    if (!have_sps) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "missing SPS");
        return -1;
    }

    length_size = (int)cfg->length_size_minus_one + 1;
    if (heic_parse_length_prefixed(ctx, data, len, length_size, &nals, &n_nals) != 0) {
        heic_pps_free(ctx, &pps);
        return -1;
    }

    /* Also allow PPS/SPS in the sample stream */
    for (i = 0; i < n_nals; i++) {
        if (nals[i].type == HEIC_NAL_PPS) {
            heic_pps_free(ctx, &pps);
            if (heic_parse_pps(ctx, nals[i].payload, nals[i].payload_len, &pps) == 0)
                have_pps = 1;
        } else if (nals[i].type == HEIC_NAL_SPS) {
            if (heic_parse_sps(ctx, nals[i].payload, nals[i].payload_len, &sps) == 0)
                have_sps = 1;
        }
    }

    if (heic_frame_alloc(ctx, out, (int)sps.pic_width_in_luma_samples,
                         (int)sps.pic_height_in_luma_samples,
                         8 + sps.bit_depth_luma_minus8, sps.chroma_format_idc) != 0) {
        heic_nals_free(ctx, nals, n_nals);
        heic_pps_free(ctx, &pps);
        return -1;
    }
    out->full_range = sps.video_full_range_flag;
    /* matrix_coeffs: 0 = GBR identity (only when colour_description present).
     * When colour description is absent or matrix is unspecified (2), match
     * libheif sRGB defaults → BT.601 (6). Old code mapped 0→BT.709 (1), which
     * diverged on streams like nokia_444 (VUI full_range only, no colour desc). */
    if (sps.colour_description_present_flag && sps.matrix_coeffs != 2)
        out->matrix_coeffs = sps.matrix_coeffs;
    else
        out->matrix_coeffs = 6;
    out->color_primaries = sps.colour_description_present_flag ? sps.colour_primaries : 1;
    out->transfer_characteristics =
        sps.colour_description_present_flag ? sps.transfer_characteristics : 13;

    if (sps.conformance_window_flag) {
        switch (sps.chroma_format_idc) {
        case 0: sub_w = 1; sub_h = 1; break;
        case 1: sub_w = 2; sub_h = 2; break;
        case 2: sub_w = 2; sub_h = 1; break;
        case 3: sub_w = 1; sub_h = 1; break;
        }
        out->crop_left = (int)(sps.conf_win_left_offset * (uint32_t)sub_w);
        out->crop_right = (int)(sps.conf_win_right_offset * (uint32_t)sub_w);
        out->crop_top = (int)(sps.conf_win_top_offset * (uint32_t)sub_h);
        out->crop_bottom = (int)(sps.conf_win_bottom_offset * (uint32_t)sub_h);
    }

    {
        int has_slice = 0;
        for (i = 0; i < n_nals; i++) {
            heic_slice_header sh;
            const uint8_t *slice_data;
            size_t slice_len;
            if (!heic_nal_is_slice(nals[i].type) || nals[i].nuh_layer_id != 0) continue;
            has_slice = 1;
            if (!have_pps) {
                heic_error(ctx, HEIC_SEVERITY_ERROR, "missing PPS");
                break;
            }
            if (heic_parse_slice_header(ctx, &nals[i], &sps, &pps, &sh) != 0) break;
            heic_error(ctx, HEIC_SEVERITY_INFO,
                       "slice hdr OK type=%d qp_y=%d data_off=%u CTUs=%u entries=%u",
                       sh.slice_type, sh.slice_qp_y, (unsigned)sh.data_offset,
                       (unsigned)sps.pic_size_in_ctbs,
                       (unsigned)sh.num_entry_point_offsets);
            if (sh.data_offset >= nals[i].payload_len) {
                heic_error(ctx, HEIC_SEVERITY_ERROR, "empty slice data");
                heic_slice_header_free(ctx, &sh);
                break;
            }
            slice_data = nals[i].payload + sh.data_offset;
            slice_len = nals[i].payload_len - sh.data_offset;
            /* Convert payload-relative EP positions to slice_data EBSP space */
            {
                uint32_t *eps = NULL;
                int ne = 0, e;
                size_t off = sh.data_offset;
                /* Count EPs in header region of RBSP (payload index < data_offset) */
                int ep_in_hdr = 0;
                for (e = 0; e < nals[i].n_ep_positions; e++) {
                    /* EP at EBSP pos p maps to RBSP pos p - e_index_before */
                    uint32_t p = nals[i].ep_positions[e];
                    uint32_t rbsp_pos = p - (uint32_t)e; /* e EPs before this one */
                    if (rbsp_pos < off) ep_in_hdr++;
                }
                for (e = 0; e < nals[i].n_ep_positions; e++) {
                    uint32_t p = nals[i].ep_positions[e];
                    uint32_t rbsp_pos = p - (uint32_t)e;
                    if (rbsp_pos < off) continue;
                    /* slice_data EBSP start = data_offset + ep_in_hdr */
                    /* EP relative to slice_data EBSP: p - (data_offset + ep_in_hdr) */
                    {
                        uint32_t slice_ebsp_start = (uint32_t)off + (uint32_t)ep_in_hdr;
                        uint32_t rel;
                        uint32_t *np;
                        if (p < slice_ebsp_start) continue;
                        rel = p - slice_ebsp_start;
                        np = (uint32_t *)heic_realloc_buf(
                            ctx, eps, (size_t)ne * sizeof(uint32_t),
                            (size_t)(ne + 1) * sizeof(uint32_t));
                        if (!np) break;
                        eps = np;
                        eps[ne++] = rel;
                    }
                }
                const heic_frame *l0[HEIC_MAX_REF_PICS] = {0};
                const heic_frame *l1[HEIC_MAX_REF_PICS] = {0};
                int n_l0 = 0, n_l1 = 0;
                int poc_bits = sps.log2_max_pic_order_cnt_lsb_minus4 + 4;
                uint32_t max_poc_lsb = 1u << poc_bits;
                int is_irap = nals[i].type >= HEIC_NAL_BLA_W_LP
                           && nals[i].type <= HEIC_NAL_CRA;
                out->poc = (int)sh.slice_pic_order_cnt_lsb;
                if (!is_irap && refs && n_refs > 0 && refs[0]
                    && refs[0]->poc_valid) {
                    uint32_t prev_lsb = (uint32_t)refs[0]->poc
                                      & (max_poc_lsb - 1u);
                    int prev_msb = refs[0]->poc - (int)prev_lsb;
                    uint32_t curr_lsb = sh.slice_pic_order_cnt_lsb;
                    if (curr_lsb < prev_lsb
                        && prev_lsb - curr_lsb >= max_poc_lsb / 2)
                        out->poc += prev_msb + (int)max_poc_lsb;
                    else if (curr_lsb > prev_lsb
                             && curr_lsb - prev_lsb > max_poc_lsb / 2)
                        out->poc += prev_msb - (int)max_poc_lsb;
                    else
                        out->poc += prev_msb;
                }
                out->poc_valid = 1;
                if (sh.slice_type != HEIC_SLICE_I) {
                    if (build_ref_lists(ctx, &sps, &sh, out->poc,
                                        refs, n_refs, l0, &n_l0,
                                        l1, &n_l1) != 0) {
                        heic_free_buf(ctx, eps);
                        heic_slice_header_free(ctx, &sh);
                        break;
                    }
                }
                if (heic_hevc_decode_slice(ctx, &sps, &pps, &sh, slice_data,
                                           slice_len, eps, ne, l0, n_l0,
                                           l1, n_l1,
                                           out, ab) == 0)
                    decode_ok = 1;
                heic_free_buf(ctx, eps);
            }
            heic_slice_header_free(ctx, &sh);
            break;
        }
        if (!has_slice)
            heic_error(ctx, HEIC_SEVERITY_ERROR, "no VCL slice NAL");
    }

    heic_nals_free(ctx, nals, n_nals);
    heic_pps_free(ctx, &pps);
    if (!decode_ok) {
        heic_frame_free(ctx, out);
        return -1;
    }
    return 0;
}

int heic_hevc_decode(heic_ctx *ctx, const heic_hvcc *cfg,
                     const uint8_t *data, size_t len,
                     heic_frame *out, const heic_abort *ab)
{
    return heic_hevc_decode_impl(ctx, cfg, data, len, NULL, 0, out, ab);
}

int heic_hevc_decode_ref(heic_ctx *ctx, const heic_hvcc *cfg,
                         const uint8_t *data, size_t len,
                         const heic_frame *ref, heic_frame *out,
                         const heic_abort *ab)
{
    const heic_frame *refs[1];
    refs[0] = ref;
    return heic_hevc_decode_impl(ctx, cfg, data, len,
                                 ref ? refs : NULL, ref ? 1 : 0, out, ab);
}

int heic_hevc_decode_refs(heic_ctx *ctx, const heic_hvcc *cfg,
                          const uint8_t *data, size_t len,
                          const heic_frame *const *refs, int n_refs,
                          heic_frame *out, const heic_abort *ab)
{
    if (n_refs < 0 || n_refs > HEIC_MAX_REF_PICS) return -1;
    return heic_hevc_decode_impl(ctx, cfg, data, len, refs, n_refs, out, ab);
}
