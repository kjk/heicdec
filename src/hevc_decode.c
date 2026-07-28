/* hevc_decode.c -- HEVC still-image entry */
#include "heic_internal.h"

/* Per-ctx cache of SPS/PPS from hvcC — grid tiles reuse one parse. */
typedef struct {
    const heic_hvcc *hvcc;
    heic_sps sps;
    heic_pps pps;
    int have_sps;
    int have_pps;
} heic_param_cache;

void heic_hevc_param_cache_free(heic_ctx *ctx)
{
    heic_param_cache *pc;
    if (!ctx || !ctx->hevc_param_cache) return;
    pc = (heic_param_cache *)ctx->hevc_param_cache;
    heic_pps_free(ctx, &pc->pps);
    heic_free_buf(ctx, pc);
    ctx->hevc_param_cache = NULL;
}

static heic_param_cache *param_cache_get(heic_ctx *ctx)
{
    heic_param_cache *pc = (heic_param_cache *)ctx->hevc_param_cache;
    if (pc) return pc;
    pc = (heic_param_cache *)heic_zalloc(ctx, sizeof(*pc));
    if (!pc) return NULL;
    ctx->hevc_param_cache = pc;
    return pc;
}

static const heic_frame *find_prev_tid0(const heic_frame *const *refs,
                                        int n_refs)
{
    int i;
    for (i = 0; i < n_refs; i++) {
        uint8_t type;
        if (!refs[i] || !refs[i]->poc_valid || refs[i]->temporal_id != 0)
            continue;
        type = refs[i]->nal_unit_type;
        if (type == HEIC_NAL_RADL_N || type == HEIC_NAL_RADL_R
            || type == HEIC_NAL_RASL_N || type == HEIC_NAL_RASL_R)
            continue;
        if (type < HEIC_NAL_BLA_W_LP && (type & 1u) == 0)
            continue;
        return refs[i];
    }
    return NULL;
}

/* DPB marks on heic_frame (see heic_internal.h). */
#define HEIC_DPB_UNMANAGED 0
#define HEIC_DPB_UNUSED    1
#define HEIC_DPB_SHORT     2
#define HEIC_DPB_LONG      3

/* Sequence callers pass mutable frame storage through const pointers; RPS
   marking updates dpb_mark so later pictures cannot match POC-wrapped frames
   that the DPB would already have unmarked (H.265 8.3.2). */
static void set_dpb_mark(const heic_frame *f, uint8_t mark)
{
    if (f) ((heic_frame *)(uintptr_t)f)->dpb_mark = mark;
}

static int refs_are_managed(const heic_frame *const *refs, int n_refs)
{
    int i;
    for (i = 0; i < n_refs; i++)
        if (refs[i] && refs[i]->dpb_mark != HEIC_DPB_UNMANAGED)
            return 1;
    return 0;
}

static int ref_is_available(const heic_frame *f, int managed)
{
    if (!f || !f->poc_valid) return 0;
    if (!managed) return 1;
    return f->dpb_mark == HEIC_DPB_SHORT || f->dpb_mark == HEIC_DPB_LONG;
}

static int nal_is_reference(uint8_t nal_type)
{
    if (nal_type >= HEIC_NAL_BLA_W_LP && nal_type <= HEIC_NAL_CRA)
        return 1;
    if (nal_type <= HEIC_NAL_RASL_R)
        return (nal_type & 1u) != 0;
    return 0;
}

static int find_ref_by_poc(const heic_frame *const *refs, int n_refs,
                           int poc, uint32_t poc_mask, int lsb_only,
                           int managed)
{
    int i;
    /* Prefer older candidates (scan reverse of newest-first harness order)
       so LSB-only LT matches the earliest still-marked picture, matching
       typical DPB insertion order in libde265/FFmpeg. */
    for (i = n_refs - 1; i >= 0; i--) {
        if (!ref_is_available(refs[i], managed)) continue;
        if ((!lsb_only && refs[i]->poc == poc)
            || (lsb_only
                && (((uint32_t)refs[i]->poc & poc_mask)
                    == ((uint32_t)poc & poc_mask))))
            return i;
    }
    return -1;
}

static int lt_poc_from_sh(const heic_slice_header *sh, int i, int curr_poc,
                          uint32_t poc_mask, int *out_poc, int *out_lsb_only)
{
    int poc = (int)sh->poc_lsb_lt[i];
    int lsb_only = !sh->delta_poc_msb_present_flag[i];
    if (!lsb_only) {
        uint64_t delta =
            (uint64_t)sh->delta_poc_msb_cycle_lt[i] *
            ((uint64_t)poc_mask + 1u);
        int curr_msb = curr_poc - (int)sh->slice_pic_order_cnt_lsb;
        int64_t full_poc;
        if (delta > INT_MAX)
            return -1;
        full_poc = (int64_t)curr_msb + poc - (int64_t)delta;
        if (full_poc < INT_MIN || full_poc > INT_MAX)
            return -1;
        poc = (int)full_poc;
    }
    *out_poc = poc;
    *out_lsb_only = lsb_only;
    return 0;
}

/* Apply H.265 8.3.2 reference marking, then build L0/L1 (8.3.4).
   Includes StFoll/LtFoll so pictures stay available for later frames. */
static int build_ref_lists(heic_ctx *ctx, const heic_sps *sps,
                           const heic_slice_header *sh,
                           int curr_poc,
                           const heic_frame *const *refs, int n_refs,
                           int clear_dpb,
                           const heic_frame **l0, int *n_l0,
                           const heic_frame **l1, int *n_l1)
{
    const heic_st_rps *rps = sh->has_inline_short_term_rps
        ? &sh->inline_short_term_rps
        : (sh->short_term_ref_pic_set_idx < sps->num_short_term_ref_pic_sets
               ? &sps->short_term_rps[sh->short_term_ref_pic_set_idx]
               : NULL);
    const heic_frame *before[HEIC_MAX_REF_PICS];
    const heic_frame *after[HEIC_MAX_REF_PICS];
    const heic_frame *lt[HEIC_MAX_REF_PICS];
    const heic_frame *temp0[HEIC_MAX_REF_PICS];
    const heic_frame *temp1[HEIC_MAX_REF_PICS];
    const heic_frame *kept[HEIC_MAX_REF_PICS * 2];
    uint8_t kept_mark[HEIC_MAX_REF_PICS * 2];
    uint32_t poc_mask =
        (1u << (sps->log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1u;
    int n_before = 0, n_after = 0, n_lt = 0, n_temp = 0, n_kept = 0;
    int managed, i;
    int need_lists = sh->slice_type != HEIC_SLICE_I;
    int do_mark;

    managed = refs_are_managed(refs, n_refs);
    if (clear_dpb) {
        for (i = 0; i < n_refs; i++)
            set_dpb_mark(refs[i], HEIC_DPB_UNUSED);
        managed = 1;
    }

    /* Short-term RPS: CurrBefore / CurrAfter / Foll (H.265 8.3.2). */
    if (rps) {
        for (i = 0; i < rps->num_negative_pics; i++) {
            int idx = find_ref_by_poc(refs, n_refs,
                                      curr_poc + rps->delta_poc_s0[i],
                                      poc_mask, 0, managed);
            if (idx < 0) continue;
            if (n_kept < (int)HEIC_COUNTOF(kept)) {
                kept[n_kept] = refs[idx];
                kept_mark[n_kept++] = HEIC_DPB_SHORT;
            }
            if (rps->used_by_curr_pic_s0[i] && n_before < HEIC_MAX_REF_PICS)
                before[n_before++] = refs[idx];
        }
        for (i = 0; i < rps->num_positive_pics; i++) {
            int idx = find_ref_by_poc(refs, n_refs,
                                      curr_poc + rps->delta_poc_s1[i],
                                      poc_mask, 0, managed);
            if (idx < 0) continue;
            if (n_kept < (int)HEIC_COUNTOF(kept)) {
                kept[n_kept] = refs[idx];
                kept_mark[n_kept++] = HEIC_DPB_SHORT;
            }
            if (rps->used_by_curr_pic_s1[i] && n_after < HEIC_MAX_REF_PICS)
                after[n_after++] = refs[idx];
        }
    }

    /* Long-term RPS: LtCurr / LtFoll. */
    for (i = 0; i < sh->num_long_term_sps + sh->num_long_term_pics; i++) {
        int idx, poc, lsb_only;
        if (lt_poc_from_sh(sh, i, curr_poc, poc_mask, &poc, &lsb_only) != 0)
            continue;
        idx = find_ref_by_poc(refs, n_refs, poc, poc_mask, lsb_only, managed);
        if (idx < 0) continue;
        if (n_kept < (int)HEIC_COUNTOF(kept)) {
            kept[n_kept] = refs[idx];
            kept_mark[n_kept++] = HEIC_DPB_LONG;
        }
        if (sh->used_by_curr_pic_lt_flag[i] && n_lt < HEIC_MAX_REF_PICS)
            lt[n_lt++] = refs[idx];
    }

    /* Commit DPB marks: drop anything not in the current RPS. Once any frame
       is managed, subsequent pictures filter LSB/POC matches through marks. */
    do_mark = managed || n_kept > 0
        || sh->num_long_term_sps + sh->num_long_term_pics > 0
        || (rps && (rps->num_negative_pics || rps->num_positive_pics));
    if (do_mark) {
        for (i = 0; i < n_refs; i++)
            set_dpb_mark(refs[i], HEIC_DPB_UNUSED);
        for (i = 0; i < n_kept; i++)
            set_dpb_mark(kept[i], kept_mark[i]);
    }

    if (!need_lists) {
        if (n_l0) *n_l0 = 0;
        if (n_l1) *n_l1 = 0;
        return 0;
    }

    /* StCurrBefore, StCurrAfter, then LtCurr (H.265 8.3.4). */
    /* Some HEIF writers leave POC metadata ambiguous across independent items.
       Preserve iref order as a bounded fallback when no POC match was possible. */
    n_temp = n_before + n_after + n_lt;
    if (n_temp == 0) {
        for (i = 0; i < n_refs && n_before < HEIC_MAX_REF_PICS; i++)
            if (refs[i] && ref_is_available(refs[i], managed))
                before[n_before++] = refs[i];
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
    heic_param_cache *pc;
    heic_sps *sps;
    heic_pps *pps;
    int have_sps = 0, have_pps = 0;
    int length_size;
    heic_nal param;
    int sub_w = 2, sub_h = 2;
    int decode_ok = 0;

    if (!ctx || !cfg || !data || !out) return -1;
    if (heic_abort_check(ab)) return -1;
    pc = param_cache_get(ctx);
    if (!pc) return -1;
    sps = &pc->sps;
    pps = &pc->pps;

    /* Reuse SPS/PPS when tiles share the same hvcC property. */
    if (pc->hvcc == cfg && pc->have_sps) {
        have_sps = 1;
        have_pps = pc->have_pps;
    } else {
        heic_pps_free(ctx, pps);
        memset(sps, 0, sizeof(*sps));
        memset(pps, 0, sizeof(*pps));
        pc->hvcc = cfg;
        pc->have_sps = 0;
        pc->have_pps = 0;
        for (i = 0; i < cfg->n_nal_units; i++) {
            if (heic_parse_single_nal(ctx, cfg->nal_units[i],
                                      cfg->nal_unit_lens[i], &param) != 0)
                continue;
            if (param.type == HEIC_NAL_SPS) {
                if (heic_parse_sps(ctx, param.payload, param.payload_len, sps)
                    == 0) {
                    have_sps = 1;
                    pc->have_sps = 1;
                }
            } else if (param.type == HEIC_NAL_PPS) {
                heic_pps_free(ctx, pps);
                if (heic_parse_pps(ctx, param.payload, param.payload_len, pps)
                    == 0) {
                    have_pps = 1;
                    pc->have_pps = 1;
                }
            }
            heic_nal_free(ctx, &param);
        }
    }
    if (!have_sps) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "missing SPS");
        return -1;
    }

    length_size = (int)cfg->length_size_minus_one + 1;
    if (heic_parse_length_prefixed(ctx, data, len, length_size, &nals, &n_nals) != 0)
        return -1;

    /* Sample-stream SPS/PPS override the cache for this decode only.
     * Invalidate hvcC reuse so the next tile reloads clean base params. */
    for (i = 0; i < n_nals; i++) {
        if (nals[i].type == HEIC_NAL_PPS) {
            heic_pps_free(ctx, pps);
            if (heic_parse_pps(ctx, nals[i].payload, nals[i].payload_len, pps)
                == 0) {
                have_pps = 1;
                pc->have_pps = 1;
            }
            pc->hvcc = NULL;
        } else if (nals[i].type == HEIC_NAL_SPS) {
            if (heic_parse_sps(ctx, nals[i].payload, nals[i].payload_len, sps)
                == 0) {
                have_sps = 1;
                pc->have_sps = 1;
            }
            pc->hvcc = NULL;
        }
    }

    /* prepare reuses plane buffers across equal-size grid tiles. */
    if (heic_frame_prepare(ctx, out, (int)sps->pic_width_in_luma_samples,
                           (int)sps->pic_height_in_luma_samples,
                           8 + sps->bit_depth_luma_minus8,
                           sps->chroma_format_idc) != 0) {
        heic_nals_free(ctx, nals, n_nals);
        return -1;
    }
    out->chroma_bit_depth = sps->chroma_format_idc
        ? 8 + sps->bit_depth_chroma_minus8 : 0;
    out->full_range = sps->video_full_range_flag;
    /* matrix_coeffs: 0 = GBR identity (only when colour_description present).
     * When colour description is absent or matrix is unspecified (2), match
     * libheif sRGB defaults → BT.601 (6). Old code mapped 0→BT.709 (1), which
     * diverged on streams like nokia_444 (VUI full_range only, no colour desc). */
    if (sps->colour_description_present_flag && sps->matrix_coeffs != 2)
        out->matrix_coeffs = sps->matrix_coeffs;
    else
        out->matrix_coeffs = 6;
    out->color_primaries =
        sps->colour_description_present_flag ? sps->colour_primaries : 1;
    out->transfer_characteristics =
        sps->colour_description_present_flag ? sps->transfer_characteristics
                                             : 13;

    if (sps->conformance_window_flag) {
        switch (sps->chroma_format_idc) {
        case 0: sub_w = 1; sub_h = 1; break;
        case 1: sub_w = 2; sub_h = 2; break;
        case 2: sub_w = 2; sub_h = 1; break;
        case 3: sub_w = 1; sub_h = 1; break;
        }
        out->crop_left = (int)(sps->conf_win_left_offset * (uint32_t)sub_w);
        out->crop_right = (int)(sps->conf_win_right_offset * (uint32_t)sub_w);
        out->crop_top = (int)(sps->conf_win_top_offset * (uint32_t)sub_h);
        out->crop_bottom = (int)(sps->conf_win_bottom_offset * (uint32_t)sub_h);
    }

    {
        heic_hevc_picture *picture = NULL;
        heic_slice_header independent;
        int have_independent = 0;
        int has_slice = 0;
        int failed = 0;
        memset(&independent, 0, sizeof(independent));
        for (i = 0; i < n_nals; i++) {
            heic_slice_header sh;
            const uint8_t *slice_data;
            const heic_frame *l0[HEIC_MAX_REF_PICS] = {0};
            const heic_frame *l1[HEIC_MAX_REF_PICS] = {0};
            uint32_t *eps = NULL;
            size_t slice_len;
            int n_l0 = 0, n_l1 = 0;
            int ne = 0, e;
            int ep_in_hdr = 0;
            size_t off;
            if (!heic_nal_is_slice(nals[i].type)
                || nals[i].nuh_layer_id != 0)
                continue;
            has_slice = 1;
            if (!have_pps) {
                heic_error(ctx, HEIC_SEVERITY_ERROR, "missing PPS");
                failed = 1;
                break;
            }
            if (heic_parse_slice_header(
                    ctx, &nals[i], sps, pps,
                    have_independent ? &independent : NULL, &sh) != 0) {
                failed = 1;
                break;
            }
            if (sh.first_slice_segment_in_pic_flag && picture) {
                heic_error(ctx, HEIC_SEVERITY_ERROR,
                           "sample contains more than one HEVC picture");
                heic_slice_header_free(ctx, &sh);
                failed = 1;
                break;
            }
            if (!sh.dependent_slice_segment_flag) {
                independent = sh;
                independent.entry_point_offsets = NULL;
                independent.num_entry_point_offsets = 0;
                have_independent = 1;
            }
            heic_error(
                ctx, HEIC_SEVERITY_INFO,
                "slice hdr OK type=%d dependent=%d address=%u qp_y=%d "
                "data_off=%u CTUs=%u entries=%u",
                sh.slice_type, sh.dependent_slice_segment_flag,
                (unsigned)sh.slice_segment_address, sh.slice_qp_y,
                (unsigned)sh.data_offset, (unsigned)sps->pic_size_in_ctbs,
                (unsigned)sh.num_entry_point_offsets);
            if (sh.data_offset >= nals[i].payload_len) {
                heic_error(ctx, HEIC_SEVERITY_ERROR, "empty slice data");
                heic_slice_header_free(ctx, &sh);
                failed = 1;
                break;
            }
            slice_data = nals[i].payload + sh.data_offset;
            slice_len = nals[i].payload_len - sh.data_offset;
            off = sh.data_offset;
            for (e = 0; e < nals[i].n_ep_positions; e++) {
                uint32_t rbsp_pos =
                    nals[i].ep_positions[e] - (uint32_t)e;
                if (rbsp_pos < off) ep_in_hdr++;
            }
            for (e = 0; e < nals[i].n_ep_positions; e++) {
                uint32_t p = nals[i].ep_positions[e];
                uint32_t rbsp_pos = p - (uint32_t)e;
                uint32_t slice_ebsp_start =
                    (uint32_t)off + (uint32_t)ep_in_hdr;
                uint32_t *np;
                if (rbsp_pos < off || p < slice_ebsp_start) continue;
                np = (uint32_t *)heic_realloc_buf(
                    ctx, eps, (size_t)ne * sizeof(uint32_t),
                    (size_t)(ne + 1) * sizeof(uint32_t));
                if (!np) {
                    failed = 1;
                    break;
                }
                eps = np;
                eps[ne++] = p - slice_ebsp_start;
            }
            if (!failed && !picture) {
                const heic_frame *prev_tid0 =
                    find_prev_tid0(refs, n_refs);
                int poc_bits =
                    sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
                uint32_t max_poc_lsb = 1u << poc_bits;
                int reset_poc =
                    nals[i].type >= HEIC_NAL_BLA_W_LP
                    && nals[i].type <= HEIC_NAL_IDR_N_LP;
                out->poc = (int)sh.slice_pic_order_cnt_lsb;
                if (!reset_poc && prev_tid0) {
                    uint32_t prev_lsb = (uint32_t)prev_tid0->poc
                                      & (max_poc_lsb - 1u);
                    int prev_msb = prev_tid0->poc - (int)prev_lsb;
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
                out->nal_unit_type = (uint8_t)nals[i].type;
                out->temporal_id = nals[i].temporal_id;
                /* Always run RPS marking (incl. I / Foll) so the caller's
                   DPB state drops POC-wrapped pictures before LSB LT match. */
                if (build_ref_lists(ctx, sps, &sh, out->poc,
                                    refs, n_refs, reset_poc,
                                    l0, &n_l0, l1, &n_l1) != 0)
                    failed = 1;
                out->dpb_mark = nal_is_reference(out->nal_unit_type)
                    ? HEIC_DPB_SHORT
                    : HEIC_DPB_UNUSED;
            } else if (!failed && sh.slice_type != HEIC_SLICE_I
                       && build_ref_lists(ctx, sps, &sh, out->poc,
                                          refs, n_refs, 0,
                                          l0, &n_l0, l1, &n_l1) != 0)
                failed = 1;
            if (!failed && !picture) {
                picture = heic_hevc_picture_new(
                    ctx, sps, pps, &sh, l0, n_l0, l1, n_l1, out);
                if (!picture) failed = 1;
            }
            if (!failed
                && heic_hevc_picture_decode_segment(
                       picture, &sh, slice_data, slice_len, eps, ne,
                       l0, n_l0, l1, n_l1, ab) != 0)
                failed = 1;
            heic_free_buf(ctx, eps);
            heic_slice_header_free(ctx, &sh);
            if (failed) break;
        }
        if (!has_slice)
            heic_error(ctx, HEIC_SEVERITY_ERROR, "no VCL slice NAL");
        if (!failed && picture && heic_hevc_picture_finish(picture) == 0)
            decode_ok = 1;
        heic_hevc_picture_destroy(picture);
    }

    heic_nals_free(ctx, nals, n_nals);
    /* PPS/SPS live in ctx param cache (shared across grid tiles). */
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
    /* refs is the decoded-picture candidate set. The slice's active lists
       remain capped at HEIC_MAX_REF_PICS after matching candidates by POC. */
    if (n_refs < 0 || n_refs > (int)HEIC_MAX_ITEMS) return -1;
    return heic_hevc_decode_impl(ctx, cfg, data, len, refs, n_refs, out, ab);
}
