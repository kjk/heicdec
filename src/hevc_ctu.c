/* hevc_ctu.c -- I-slice CTU / CU / transform tree (port of imazen/heic ctu.rs) */
#include "heic_internal.h"

typedef struct {
    heic_ctx *hctx;
    const heic_sps *sps;
    const heic_pps *pps;
    const heic_slice_header *sh;
    heic_cabac cabac;
    heic_ctx_model models[HEIC_NUM_CONTEXTS];
    heic_frame *frame;

    uint32_t ctb_x, ctb_y;
    int qp_y, qp_cb, qp_cr;
    int is_cu_qp_delta_coded;
    int cu_qp_delta;
    int cu_transquant_bypass;
    uint32_t cu_base_x, cu_base_y;
    uint8_t cu_log2_size;

    uint8_t *ct_depth_map;
    uint32_t ct_depth_stride;
    size_t   ct_depth_n;
    uint8_t *intra_mode_map;
    uint8_t *intra_chroma_mode_map;
    uint32_t intra_mode_stride;
    size_t   intra_mode_n;
    int8_t *qp_map;
    uint32_t qp_map_stride;
    size_t   qp_map_n;
    int current_qpy;
    int last_qpy_in_prev_qg;
    int current_qg_x, current_qg_y;

    heic_sao_info *sao_map;
    uint32_t sao_stride; /* pic_width_in_ctbs */

    /* Deblock maps at 4x4 luma granularity */
    uint8_t *deblock_flags;
    int8_t  *deblock_qp;
    uint32_t deblock_stride;
    uint32_t deblock_n;

    int16_t *residual_buf; /* HEIC_MAX_COEFF, heap (keep CTU frame small for ASan) */
    heic_coeff_buf *coeff; /* residual decode scratch (2KB; not on recursive stack) */
} heic_slice_ctx;

static int bit_depth_y(const heic_sps *s) { return 8 + s->bit_depth_luma_minus8; }
static int bit_depth_c(const heic_sps *s) { return 8 + s->bit_depth_chroma_minus8; }
static uint32_t ctb_size_px(const heic_sps *s) { return 1u << s->log2_ctb_size; }
static uint32_t min_pu_size(const heic_sps *s)
{
    uint32_t m = (1u << s->log2_min_cb_size) / 2;
    return m ? m : 1;
}

static int chroma_array_type(const heic_sps *s)
{
    return s->separate_colour_plane_flag ? 0 : (int)s->chroma_format_idc;
}

/* H.265 8.6.1: Table 8-10 only for ChromaArrayType==1 (4:2:0).
   For 4:2:2 / 4:4:4, QpC = Min(qPi, 51). */
static int chroma_qp_from_luma(int qpi, int chroma_array_type)
{
    static const int TAB[13] = {29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37};
    if (chroma_array_type != 1) {
        if (qpi < 0) return 0;
        if (qpi > 51) return 51;
        return qpi;
    }
    if (qpi < 30) return qpi;
    if (qpi >= 43) return qpi - 6;
    return TAB[qpi - 30];
}

static int neighbor_avail(const heic_slice_ctx *sc, int32_t x, int32_t y)
{
    if (x < 0 || y < 0) return 0;
    if ((uint32_t)x >= sc->sps->pic_width_in_luma_samples) return 0;
    if ((uint32_t)y >= sc->sps->pic_height_in_luma_samples) return 0;
    return 1;
}

static uint8_t get_ct_depth(const heic_slice_ctx *sc, uint32_t x, uint32_t y)
{
    uint32_t min_cb = 1u << sc->sps->log2_min_cb_size;
    uint32_t mx = x / min_cb, my = y / min_cb;
    size_t idx;
    if (mx >= sc->ct_depth_stride) return 0xFF;
    idx = (size_t)my * sc->ct_depth_stride + mx;
    if (idx >= sc->ct_depth_n) return 0xFF;
    return sc->ct_depth_map[idx];
}

static void set_ct_depth(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                        uint8_t log2_cb, uint8_t depth)
{
    uint32_t min_cb = 1u << sc->sps->log2_min_cb_size;
    uint32_t cb = 1u << log2_cb;
    uint32_t sx = x0 / min_cb, sy = y0 / min_cb, n = cb / min_cb, dx, dy;
    for (dy = 0; dy < n; dy++)
        for (dx = 0; dx < n; dx++) {
            uint32_t mx = sx + dx, my = sy + dy;
            size_t idx;
            if (mx >= sc->ct_depth_stride) continue;
            idx = (size_t)my * sc->ct_depth_stride + mx;
            if (idx >= sc->ct_depth_n) continue;
            sc->ct_depth_map[idx] = depth;
        }
}

static void store_intra_mode(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                             uint8_t log2_size, uint8_t mode, int chroma)
{
    uint32_t mpu = min_pu_size(sc->sps);
    uint32_t count = ((1u << log2_size) / mpu);
    uint32_t sx = x0 / mpu, sy = y0 / mpu, dx, dy;
    uint8_t *map = chroma ? sc->intra_chroma_mode_map : sc->intra_mode_map;
    if (count == 0) count = 1;
    for (dy = 0; dy < count; dy++)
        for (dx = 0; dx < count; dx++) {
            size_t idx = (size_t)(sy + dy) * sc->intra_mode_stride + (sx + dx);
            /* CU may extend past pic edge (partial last CTB); clip to map. */
            if (idx < sc->intra_mode_n) map[idx] = mode;
        }
}

static uint8_t get_intra_mode(const heic_slice_ctx *sc, uint32_t x, uint32_t y, int chroma)
{
    uint32_t mpu = min_pu_size(sc->sps);
    size_t idx = (size_t)(y / mpu) * sc->intra_mode_stride + (x / mpu);
    const uint8_t *map = chroma ? sc->intra_chroma_mode_map : sc->intra_mode_map;
    if (idx >= sc->intra_mode_n) return 1; /* DC */
    return map[idx];
}

static uint8_t neighbor_intra_left(const heic_slice_ctx *sc, uint32_t x0, uint32_t y0)
{
    if (x0 == 0) return 1; /* DC */
    return get_intra_mode(sc, x0 - 1, y0, 0);
}

static uint8_t neighbor_intra_above(const heic_slice_ctx *sc, uint32_t x0, uint32_t y0)
{
    uint32_t ctb = ctb_size_px(sc->sps);
    uint32_t ctb_y0;
    if (y0 == 0) return 1;
    ctb_y0 = (y0 / ctb) * ctb;
    if (y0 - 1 < ctb_y0) return 1;
    return get_intra_mode(sc, x0, y0 - 1, 0);
}

static void store_qpy(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                      uint8_t log2_cb, int qpy)
{
    uint32_t min_tb = 1u << sc->sps->log2_min_tb_size;
    uint32_t count = ((1u << log2_cb) / min_tb);
    uint32_t sx = x0 / min_tb, sy = y0 / min_tb, dx, dy;
    if (count == 0) count = 1;
    for (dy = 0; dy < count; dy++)
        for (dx = 0; dx < count; dx++) {
            size_t idx = (size_t)(sy + dy) * sc->qp_map_stride + (sx + dx);
            /* Partial last CTB: CU footprint can extend past pic (and map). */
            if (idx < sc->qp_map_n) sc->qp_map[idx] = (int8_t)qpy;
        }
}

static int get_qpy_at(const heic_slice_ctx *sc, uint32_t x, uint32_t y)
{
    uint32_t min_tb = 1u << sc->sps->log2_min_tb_size;
    size_t idx = (size_t)(y / min_tb) * sc->qp_map_stride + (x / min_tb);
    if (idx >= sc->qp_map_n) return sc->sh->slice_qp_y;
    return sc->qp_map[idx];
}

static void decode_quant_params(heic_slice_ctx *sc, uint32_t x0,
                                uint32_t x_cu, uint32_t y_cu)
{
    /* saturating: crafted PPS can set diff_cu_qp_delta_depth > log2_ctb_size */
    uint8_t log2_min_qg =
        sc->sps->log2_ctb_size > sc->pps->diff_cu_qp_delta_depth
            ? (uint8_t)(sc->sps->log2_ctb_size - sc->pps->diff_cu_qp_delta_depth)
            : 0;
    uint32_t qg_mask = (1u << log2_min_qg) - 1;
    int x_qg = (int)(x_cu & ~qg_mask);
    int y_qg = (int)(y_cu & ~qg_mask);
    int ctb_mask = (int)((1u << sc->sps->log2_ctb_size) - 1);
    int first_in_ctb_row, first_qg_in_slice;
    int qp_y_pred, qp_y_a, qp_y_b, qp_bd_y, qp_bd_c, qpy, qpi_cb, qpi_cr;
    uint32_t slice_sx, slice_sy;

    if (x_qg != sc->current_qg_x || y_qg != sc->current_qg_y) {
        sc->last_qpy_in_prev_qg = sc->current_qpy;
        sc->current_qg_x = x_qg;
        sc->current_qg_y = y_qg;
    }

    first_in_ctb_row = (x_qg == 0 && (y_qg & ctb_mask) == 0);
    slice_sx = (sc->sh->slice_segment_address % sc->sps->pic_width_in_ctbs)
               * ctb_size_px(sc->sps);
    slice_sy = (sc->sh->slice_segment_address / sc->sps->pic_width_in_ctbs)
               * ctb_size_px(sc->sps);
    first_qg_in_slice = ((int)slice_sx == x_qg && (int)slice_sy == y_qg);

    if (first_qg_in_slice
        || (first_in_ctb_row && sc->pps->entropy_coding_sync_enabled_flag))
        qp_y_pred = sc->sh->slice_qp_y;
    else
        qp_y_pred = sc->last_qpy_in_prev_qg;

    if (x_qg > 0) {
        uint32_t lx = (uint32_t)(x_qg - 1), ly = (uint32_t)y_qg;
        uint32_t ctb = ctb_size_px(sc->sps);
        if (lx / ctb == sc->ctb_x)
            qp_y_a = get_qpy_at(sc, lx, ly);
        else
            qp_y_a = qp_y_pred;
        (void)x0;
    } else {
        qp_y_a = qp_y_pred;
    }
    if (y_qg > 0) {
        uint32_t ax = (uint32_t)x_qg, ay = (uint32_t)(y_qg - 1);
        uint32_t ctb = ctb_size_px(sc->sps);
        if (ay / ctb == sc->ctb_y)
            qp_y_b = get_qpy_at(sc, ax, ay);
        else
            qp_y_b = qp_y_pred;
    } else {
        qp_y_b = qp_y_pred;
    }
    qp_y_pred = (qp_y_a + qp_y_b + 1) >> 1;

    qp_bd_y = 6 * (bit_depth_y(sc->sps) - 8);
    qpy = ((qp_y_pred + sc->cu_qp_delta + 52 + 2 * qp_bd_y) % (52 + qp_bd_y))
          - qp_bd_y;
    sc->qp_y = qpy + qp_bd_y;
    if (sc->qp_y < 0) sc->qp_y = 0;

    qp_bd_c = 6 * (bit_depth_c(sc->sps) - 8);
    qpi_cb = qpy + sc->pps->pps_cb_qp_offset + sc->sh->slice_cb_qp_offset;
    qpi_cr = qpy + sc->pps->pps_cr_qp_offset + sc->sh->slice_cr_qp_offset;
    if (qpi_cb < -qp_bd_c) qpi_cb = -qp_bd_c;
    if (qpi_cb > 57) qpi_cb = 57;
    if (qpi_cr < -qp_bd_c) qpi_cr = -qp_bd_c;
    if (qpi_cr > 57) qpi_cr = 57;
    {
        int cat = chroma_array_type(sc->sps);
        sc->qp_cb = chroma_qp_from_luma(qpi_cb, cat) + qp_bd_c;
        sc->qp_cr = chroma_qp_from_luma(qpi_cr, cat) + qp_bd_c;
    }
    sc->current_qpy = qpy;
}

static int decode_split_cu(heic_slice_ctx *sc, uint32_t x0, uint32_t y0, uint8_t ct_depth)
{
    int cond_l = 0, cond_a = 0, ctx_idx, bin;
    if (neighbor_avail(sc, (int32_t)x0 - 1, (int32_t)y0)) {
        uint8_t d = get_ct_depth(sc, x0 - 1, y0);
        if (d != 0xFF && d > ct_depth) cond_l = 1;
    }
    if (neighbor_avail(sc, (int32_t)x0, (int32_t)y0 - 1)) {
        uint8_t d = get_ct_depth(sc, x0, y0 - 1);
        if (d != 0xFF && d > ct_depth) cond_a = 1;
    }
    ctx_idx = HEIC_CTX_SPLIT_CU_FLAG + cond_l + cond_a;
    bin = heic_cabac_decode_bin(&sc->cabac, &sc->models[ctx_idx]);
    return bin != 0;
}

static int decode_part_mode_intra(heic_slice_ctx *sc, uint8_t log2_cb)
{
    int bin = heic_cabac_decode_bin(&sc->cabac, &sc->models[HEIC_CTX_PART_MODE]);
    if (bin != 0) return 0; /* 2Nx2N */
    if (log2_cb == sc->sps->log2_min_cb_size) return 1; /* NxN */
    return -1;
}

static int decode_prev_intra_flag(heic_slice_ctx *sc)
{
    return heic_cabac_decode_bin(&sc->cabac,
                                 &sc->models[HEIC_CTX_PREV_INTRA_LUMA_PRED_FLAG])
           != 0;
}

static uint8_t decode_mpm_idx(heic_slice_ctx *sc)
{
    if (heic_cabac_decode_bypass(&sc->cabac) == 0) return 0;
    if (heic_cabac_decode_bypass(&sc->cabac) == 0) return 1;
    return 2;
}

static uint32_t decode_rem_intra(heic_slice_ctx *sc)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 5; i++)
        v = (v << 1) | (uint32_t)heic_cabac_decode_bypass(&sc->cabac);
    return v;
}

static uint8_t map_rem_mode(uint32_t rem, const uint8_t mpm[3])
{
    uint8_t sorted[3] = {mpm[0], mpm[1], mpm[2]};
    uint8_t mode;
    int i, j;
    for (i = 0; i < 2; i++)
        for (j = i + 1; j < 3; j++)
            if (sorted[i] > sorted[j]) {
                uint8_t t = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = t;
            }
    mode = (uint8_t)rem;
    for (i = 0; i < 3; i++)
        if (mode >= sorted[i]) mode++;
    return mode;
}

static uint8_t derive_intra_luma(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                                 int prev_flag)
{
    uint8_t cand_a = neighbor_intra_left(sc, x0, y0);
    uint8_t cand_b = neighbor_intra_above(sc, x0, y0);
    uint8_t mpm[3];
    heic_fill_mpm(cand_a, cand_b, mpm);
    if (prev_flag) return mpm[decode_mpm_idx(sc)];
    return map_rem_mode(decode_rem_intra(sc), mpm);
}

static uint8_t decode_intra_chroma(heic_slice_ctx *sc, uint8_t luma)
{
    int first;
    uint32_t mode_idx;
    uint8_t cand;
    if (chroma_array_type(sc->sps) == 0) return luma;
    first = heic_cabac_decode_bin(&sc->cabac,
                                  &sc->models[HEIC_CTX_INTRA_CHROMA_PRED_MODE]);
    if (first == 0) return luma;
    mode_idx = heic_cabac_decode_bypass_bits(&sc->cabac, 2);
    if (mode_idx == 0) cand = 0;
    else if (mode_idx == 1) cand = 26;
    else if (mode_idx == 2) cand = 10;
    else cand = 1;
    if (cand == luma) return 34;
    return cand;
}

static uint32_t decode_cu_qp_delta_abs(heic_slice_ctx *sc)
{
    int first = heic_cabac_decode_bin(&sc->cabac, &sc->models[HEIC_CTX_CU_QP_DELTA_ABS]);
    uint32_t prefix;
    int i;
    if (first == 0) return 0;
    prefix = 1;
    for (i = 0; i < 4; i++) {
        int bin = heic_cabac_decode_bin(&sc->cabac, &sc->models[HEIC_CTX_CU_QP_DELTA_ABS + 1]);
        if (bin == 0) break;
        prefix++;
    }
    if (prefix == 5) {
        uint32_t suffix = heic_cabac_decode_egk(&sc->cabac, 0);
        return suffix + 5;
    }
    return prefix;
}

static int decode_and_apply_residual(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                                     uint8_t log2_size, uint8_t c_idx, int scan_order)
{
    heic_coeff_buf *coeff = sc->coeff;
    int transform_skip = 0;
    int size, num, qp, bd, is_intra_4x4, max_val;
    uint16_t *plane;
    int stride, plane_w, plane_h;

    if (!coeff) return -1;
    if (heic_decode_residual(&sc->cabac, sc->models, log2_size, c_idx, scan_order,
                             sc->pps->sign_data_hiding_enabled_flag,
                             sc->cu_transquant_bypass,
                             sc->pps->transform_skip_enabled_flag,
                             coeff, &transform_skip)
        != 0)
        return -1;
    if (coeff->num_nonzero == 0) return 0;

    size = 1 << log2_size;
    num = size * size;
    if (c_idx == 0) {
        qp = sc->qp_y;
        bd = bit_depth_y(sc->sps);
        plane = sc->frame->y;
        stride = sc->frame->y_stride;
        plane_w = sc->frame->width;
        plane_h = sc->frame->height;
    } else if (c_idx == 1) {
        qp = sc->qp_cb;
        bd = bit_depth_c(sc->sps);
        plane = sc->frame->cb;
        stride = sc->frame->c_stride;
        plane_w = sc->frame->c_width;
        plane_h = sc->frame->c_height;
    } else {
        qp = sc->qp_cr;
        bd = bit_depth_c(sc->sps);
        plane = sc->frame->cr;
        stride = sc->frame->c_stride;
        plane_w = sc->frame->c_width;
        plane_h = sc->frame->c_height;
    }
    if (!plane) return 0;
    max_val = (1 << bd) - 1;

    if (sc->cu_transquant_bypass) {
        int py, px;
        memcpy(sc->residual_buf, coeff->coeffs, (size_t)num * sizeof(int16_t));
        for (py = 0; py < size; py++) {
            if ((int)y0 + py >= plane_h) break;
            for (px = 0; px < size; px++) {
                int32_t v;
                if ((int)x0 + px >= plane_w) break;
                v = (int32_t)plane[((int)y0 + py) * stride + (int)x0 + px]
                    + sc->residual_buf[py * size + px];
                if (v < 0) v = 0;
                if (v > max_val) v = max_val;
                plane[((int)y0 + py) * stride + (int)x0 + px] = (uint16_t)v;
            }
        }
        return 0;
    }

    heic_dequantize(coeff->coeffs, num, qp, bd, log2_size);

    if (transform_skip) {
        int ts_shift = 5 + (int)log2_size;
        int bd_shift = 20 - bd;
        int rnd, i;
        if (bd_shift < 0) bd_shift = 0;
        rnd = bd_shift > 0 ? (1 << (bd_shift - 1)) : 0;
        for (i = 0; i < num; i++) {
            int32_t c = ((int32_t)coeff->coeffs[i] << ts_shift);
            sc->residual_buf[i] = (int16_t)((c + rnd) >> bd_shift);
        }
    } else {
        is_intra_4x4 = (log2_size == 2 && c_idx == 0);
        heic_inverse_transform(coeff->coeffs, sc->residual_buf, size, bd, is_intra_4x4);
    }

    /* Clip add if block may extend past plane edge. */
    if ((int)x0 + size <= plane_w && (int)y0 + size <= plane_h) {
        heic_add_residual(plane, stride, (int)x0, (int)y0, sc->residual_buf, size,
                          max_val);
    } else {
        int py, px;
        for (py = 0; py < size; py++) {
            if ((int)y0 + py >= plane_h) break;
            for (px = 0; px < size; px++) {
                int32_t v;
                if ((int)x0 + px >= plane_w) break;
                v = (int32_t)plane[((int)y0 + py) * stride + (int)x0 + px]
                    + sc->residual_buf[py * size + px];
                if (v < 0) v = 0;
                if (v > max_val) v = max_val;
                plane[((int)y0 + py) * stride + (int)x0 + px] = (uint16_t)v;
            }
        }
    }
    return 0;
}

static int decode_tu_leaf(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                          uint8_t log2_size, uint8_t trafo_depth, int cbf_cb,
                          int cbf_cr);
static int decode_tt_inner(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                           uint8_t log2_size, uint8_t trafo_depth,
                           int intra_split, int cbf_cb_parent, int cbf_cr_parent);

static int decode_tu_leaf(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                          uint8_t log2_size, uint8_t trafo_depth, int cbf_cb,
                          int cbf_cr)
{
    int cbf_luma, sis, scan, chroma_here, is_444;
    uint8_t luma_mode, chroma_mode;
    int ctx_off, ctx_idx;

    /* I-slice always MODE_INTRA → cbf_luma always coded */
    ctx_off = trafo_depth == 0 ? 1 : 0;
    ctx_idx = HEIC_CTX_CBF_LUMA + ctx_off;
    cbf_luma = heic_cabac_decode_bin(&sc->cabac, &sc->models[ctx_idx]) != 0;

    if ((cbf_luma || cbf_cb || cbf_cr) && sc->pps->cu_qp_delta_enabled_flag
        && !sc->is_cu_qp_delta_coded) {
        uint32_t absd = decode_cu_qp_delta_abs(sc);
        int sign = 0;
        int64_t delta, qp_bd;
        if (absd != 0) sign = heic_cabac_decode_bypass(&sc->cabac);
        sc->is_cu_qp_delta_coded = 1;
        delta = (int64_t)absd * (1 - 2 * (int64_t)sign);
        qp_bd = 6 * ((int64_t)bit_depth_y(sc->sps) - 8);
        if (delta < -(26 + qp_bd / 2) || delta > 25 + qp_bd / 2) return -1;
        sc->cu_qp_delta = (int)delta;
        decode_quant_params(sc, x0, sc->cu_base_x, sc->cu_base_y);
        store_qpy(sc, sc->cu_base_x, sc->cu_base_y, sc->cu_log2_size, sc->current_qpy);
    }

    luma_mode = get_intra_mode(sc, x0, y0, 0);
    sis = sc->sps->strong_intra_smoothing_enabled_flag;
    if (heic_predict_intra(sc->frame, x0, y0, log2_size, luma_mode, 0, sis) != 0)
        return -1;
    scan = heic_get_scan_order(log2_size, luma_mode, 0, 0);
    if (cbf_luma) {
        if (decode_and_apply_residual(sc, x0, y0, log2_size, 0, scan) != 0)
            return -1;
    }

    /* Mark TU edges + QP for deblocking (4x4 grid) */
    {
        uint32_t tu_size = 1u << log2_size;
        heic_mark_tu_boundary(sc->deblock_flags, sc->deblock_stride, sc->deblock_n,
                              x0, y0, tu_size);
        heic_store_deblock_qp(sc->deblock_qp, sc->deblock_stride, sc->deblock_n,
                              x0, y0, tu_size, (int8_t)sc->current_qpy);
    }

    is_444 = sc->frame->chroma_format == 3;
    chroma_here = is_444 || log2_size >= 3;
    if (chroma_here && chroma_array_type(sc->sps) != 0) {
        uint8_t clog2;
        uint32_t cx, cy;
        int cscan;
        if (is_444) {
            clog2 = log2_size;
            cx = x0;
            cy = y0;
        } else {
            clog2 = (uint8_t)(log2_size - 1);
            cx = x0 / 2;
            cy = y0 / 2;
        }
        chroma_mode = get_intra_mode(sc, x0, y0, 1);
        cscan = heic_get_scan_order(clog2, chroma_mode, 1, is_444);
        if (heic_predict_intra(sc->frame, cx, cy, clog2, chroma_mode, 1, sis) != 0)
            return -1;
        if (cbf_cb) {
            if (decode_and_apply_residual(sc, cx, cy, clog2, 1, cscan) != 0)
                return -1;
        }
        if (heic_predict_intra(sc->frame, cx, cy, clog2, chroma_mode, 2, sis) != 0)
            return -1;
        if (cbf_cr) {
            if (decode_and_apply_residual(sc, cx, cy, clog2, 2, cscan) != 0)
                return -1;
        }
    }
    return 0;
}

static int decode_tt_inner(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                           uint8_t log2_size, uint8_t trafo_depth,
                           int intra_split, int cbf_cb_parent, int cbf_cr_parent)
{
    uint8_t max_depth = (uint8_t)(sc->sps->max_transform_hierarchy_depth_intra
                                  + (intra_split ? 1 : 0));
    uint8_t log2_min = sc->sps->log2_min_tb_size;
    uint8_t log2_max = sc->sps->log2_max_tb_size;
    int split, cbf_cb, cbf_cr, cat;

    /* TB size is 4..32 (log2 2..5); refuse wrap / absurd depth. */
    if (log2_size < 2 || log2_size > 6 || trafo_depth > 8) return -1;
    if (log2_min < 2) log2_min = 2;
    if (log2_max > 5) log2_max = 5;
    if (log2_max < log2_min) log2_max = log2_min;

    if (log2_size <= log2_max && log2_size > log2_min && trafo_depth < max_depth
        && !(intra_split && trafo_depth == 0)) {
        int ctx = HEIC_CTX_SPLIT_TRANSFORM_FLAG
                  + ((5 - (int)log2_size) < 2 ? (5 - (int)log2_size) : 2);
        if (ctx < HEIC_CTX_SPLIT_TRANSFORM_FLAG) ctx = HEIC_CTX_SPLIT_TRANSFORM_FLAG;
        split = heic_cabac_decode_bin(&sc->cabac, &sc->models[ctx]) != 0;
    } else if ((log2_size > log2_max || (intra_split && trafo_depth == 0))
               && log2_size > log2_min) {
        /* Force split only while we can still shrink without underflow. */
        split = 1;
    } else {
        split = 0;
    }

    cat = chroma_array_type(sc->sps);
    if (cat == 0) {
        cbf_cb = 0;
        cbf_cr = 0;
    } else if (log2_size > 2 || cat == 3) {
        if (trafo_depth == 0 || cbf_cb_parent) {
            int ctx = HEIC_CTX_CBF_CBCR + trafo_depth;
            cbf_cb = heic_cabac_decode_bin(&sc->cabac, &sc->models[ctx]) != 0;
        } else
            cbf_cb = 0;
        if (trafo_depth == 0 || cbf_cr_parent) {
            int ctx = HEIC_CTX_CBF_CBCR + trafo_depth;
            cbf_cr = heic_cabac_decode_bin(&sc->cabac, &sc->models[ctx]) != 0;
        } else
            cbf_cr = 0;
    } else {
        cbf_cb = cbf_cb_parent;
        cbf_cr = cbf_cr_parent;
    }

    if (split) {
        uint32_t half = 1u << (log2_size - 1);
        uint8_t nd = (uint8_t)(trafo_depth + 1);
        uint8_t nl = (uint8_t)(log2_size - 1);
        if (decode_tt_inner(sc, x0, y0, nl, nd, intra_split, cbf_cb, cbf_cr) != 0)
            return -1;
        if (decode_tt_inner(sc, x0 + half, y0, nl, nd, intra_split, cbf_cb, cbf_cr) != 0)
            return -1;
        if (decode_tt_inner(sc, x0, y0 + half, nl, nd, intra_split, cbf_cb, cbf_cr) != 0)
            return -1;
        if (decode_tt_inner(sc, x0 + half, y0 + half, nl, nd, intra_split, cbf_cb, cbf_cr)
            != 0)
            return -1;

        /* 4:2:0 chroma at 8x8→4x4 split */
        if (log2_size == 3 && sc->frame->chroma_format != 3 && cat != 0) {
            int sis = sc->sps->strong_intra_smoothing_enabled_flag;
            uint8_t cm = get_intra_mode(sc, x0, y0, 1);
            int scan = heic_get_scan_order(2, cm, 1, 0);
            if (heic_predict_intra(sc->frame, x0 / 2, y0 / 2, 2, cm, 1, sis) != 0)
                return -1;
            if (cbf_cb
                && decode_and_apply_residual(sc, x0 / 2, y0 / 2, 2, 1, scan) != 0)
                return -1;
            if (heic_predict_intra(sc->frame, x0 / 2, y0 / 2, 2, cm, 2, sis) != 0)
                return -1;
            if (cbf_cr
                && decode_and_apply_residual(sc, x0 / 2, y0 / 2, 2, 2, scan) != 0)
                return -1;
        }
    } else {
        if (decode_tu_leaf(sc, x0, y0, log2_size, trafo_depth, cbf_cb, cbf_cr) != 0)
            return -1;
    }
    return 0;
}

static int decode_coding_unit(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                              uint8_t log2_cb, uint8_t ct_depth)
{
    uint32_t cb_size = 1u << log2_cb;
    int part_nxn = 0;
    uint8_t luma0 = 1, chroma = 1;
    int intra_split;

    sc->cu_base_x = x0;
    sc->cu_base_y = y0;
    sc->cu_log2_size = log2_cb;
    decode_quant_params(sc, x0, x0, y0);
    store_qpy(sc, x0, y0, log2_cb, sc->current_qpy);
    set_ct_depth(sc, x0, y0, log2_cb, ct_depth);

    sc->cu_transquant_bypass = 0;
    if (sc->pps->transquant_bypass_enabled_flag)
        sc->cu_transquant_bypass =
            heic_cabac_decode_bin(&sc->cabac,
                                  &sc->models[HEIC_CTX_CU_TRANSQUANT_BYPASS_FLAG])
            != 0;

    /* PCM: if enabled and size in range, check pcm_flag via terminate */
    if (sc->sps->pcm_enabled_flag) {
        uint8_t log2_min = (uint8_t)(sc->sps->log2_min_pcm_luma_coding_block_size_minus3
                                     + 3);
        uint8_t log2_max =
            (uint8_t)(log2_min + sc->sps->log2_diff_max_min_pcm_luma_coding_block_size);
        if (log2_cb >= log2_min && log2_cb <= log2_max) {
            if (heic_cabac_decode_terminate(&sc->cabac) != 0) {
                heic_error(sc->hctx, HEIC_SEVERITY_ERROR, "PCM mode not supported");
                return -1;
            }
        }
    }

    if (log2_cb == sc->sps->log2_min_cb_size) {
        int pm = decode_part_mode_intra(sc, log2_cb);
        if (pm < 0) return -1;
        part_nxn = pm;
    }

    if (!part_nxn) {
        int prev = decode_prev_intra_flag(sc);
        luma0 = derive_intra_luma(sc, x0, y0, prev);
        store_intra_mode(sc, x0, y0, log2_cb, luma0, 0);
        chroma = decode_intra_chroma(sc, luma0);
        store_intra_mode(sc, x0, y0, log2_cb, chroma, 1);
    } else {
        uint32_t half = cb_size / 2;
        uint8_t log2_pu = (uint8_t)(log2_cb - 1);
        int pf[4];
        uint8_t lm[4];
        int i;
        for (i = 0; i < 4; i++) pf[i] = decode_prev_intra_flag(sc);
        lm[0] = derive_intra_luma(sc, x0, y0, pf[0]);
        store_intra_mode(sc, x0, y0, log2_pu, lm[0], 0);
        lm[1] = derive_intra_luma(sc, x0 + half, y0, pf[1]);
        store_intra_mode(sc, x0 + half, y0, log2_pu, lm[1], 0);
        lm[2] = derive_intra_luma(sc, x0, y0 + half, pf[2]);
        store_intra_mode(sc, x0, y0 + half, log2_pu, lm[2], 0);
        lm[3] = derive_intra_luma(sc, x0 + half, y0 + half, pf[3]);
        store_intra_mode(sc, x0 + half, y0 + half, log2_pu, lm[3], 0);
        if (chroma_array_type(sc->sps) == 3) {
            uint8_t cm;
            cm = decode_intra_chroma(sc, lm[0]);
            store_intra_mode(sc, x0, y0, log2_pu, cm, 1);
            cm = decode_intra_chroma(sc, lm[1]);
            store_intra_mode(sc, x0 + half, y0, log2_pu, cm, 1);
            cm = decode_intra_chroma(sc, lm[2]);
            store_intra_mode(sc, x0, y0 + half, log2_pu, cm, 1);
            cm = decode_intra_chroma(sc, lm[3]);
            store_intra_mode(sc, x0 + half, y0 + half, log2_pu, cm, 1);
            chroma = cm;
        } else {
            chroma = decode_intra_chroma(sc, lm[0]);
            store_intra_mode(sc, x0, y0, log2_cb, chroma, 1);
        }
        luma0 = lm[0];
        /* Intra PART_NxN: internal PB edges for deblock (I-slice bS still 2). */
        if (sc->deblock_flags) {
            heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride, sc->deblock_n,
                                  x0 + half, y0, half, cb_size, 1);
            heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride, sc->deblock_n,
                                  x0, y0 + half, cb_size, half, 0);
        }
    }
    (void)luma0;
    (void)chroma;

    intra_split = part_nxn;
    return decode_tt_inner(sc, x0, y0, log2_cb, 0, intra_split, 1, 1);
}

static int decode_cqt(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                      uint8_t log2_cb, uint8_t ct_depth)
{
    uint32_t cb;
    uint32_t pw = sc->sps->pic_width_in_luma_samples;
    uint32_t ph = sc->sps->pic_height_in_luma_samples;
    uint8_t log2_min = sc->sps->log2_min_cb_size;
    uint8_t log2_qg;
    int split;

    /* Hard bounds: prevent log2_cb-1 wrap (255) infinite recursion. */
    if (log2_cb < 3 || log2_cb > 6 || log2_cb < log2_min || ct_depth > 6)
        return -1;
    cb = 1u << log2_cb;

    if (x0 + cb <= pw && y0 + cb <= ph && log2_cb > log2_min)
        split = decode_split_cu(sc, x0, y0, ct_depth);
    else if (log2_cb > log2_min)
        split = 1;
    else
        split = 0;

    log2_qg = sc->sps->log2_ctb_size > sc->pps->diff_cu_qp_delta_depth
                  ? (uint8_t)(sc->sps->log2_ctb_size - sc->pps->diff_cu_qp_delta_depth)
                  : 0;
    if (sc->pps->cu_qp_delta_enabled_flag && log2_cb >= log2_qg) {
        sc->is_cu_qp_delta_coded = 0;
        sc->cu_qp_delta = 0;
    }

    if (split) {
        uint32_t half = cb / 2;
        uint32_t x1 = x0 + half, y1 = y0 + half;
        uint8_t child = (uint8_t)(log2_cb - 1);
        uint8_t nd = (uint8_t)(ct_depth + 1);
        if (decode_cqt(sc, x0, y0, child, nd) != 0) return -1;
        if (x1 < pw && decode_cqt(sc, x1, y0, child, nd) != 0) return -1;
        if (y1 < ph && decode_cqt(sc, x0, y1, child, nd) != 0) return -1;
        if (x1 < pw && y1 < ph && decode_cqt(sc, x1, y1, child, nd) != 0) return -1;
    } else {
        if (decode_coding_unit(sc, x0, y0, log2_cb, ct_depth) != 0) return -1;
    }
    return 0;
}

static uint8_t decode_sao_type_idx(heic_slice_ctx *sc)
{
    int b0 = heic_cabac_decode_bin(&sc->cabac, &sc->models[HEIC_CTX_SAO_TYPE_IDX]);
    if (b0 == 0) return 0;
    return heic_cabac_decode_bypass(&sc->cabac) == 0 ? 1 : 2;
}

static uint32_t decode_tu_bypass(heic_slice_ctx *sc, uint32_t c_max)
{
    uint32_t i;
    for (i = 0; i < c_max; i++) {
        if (heic_cabac_decode_bypass(&sc->cabac) == 0) return i;
    }
    return c_max;
}

static int decode_sao(heic_slice_ctx *sc, uint32_t x_ctb_px, uint32_t y_ctb_px)
{
    uint32_t ctb = ctb_size_px(sc->sps);
    uint32_t x_ctb = x_ctb_px / ctb, y_ctb = y_ctb_px / ctb;
    int merge_left = 0, merge_up = 0;
    heic_sao_info info;
    uint32_t pic_w = sc->sps->pic_width_in_ctbs;
    uint32_t addr_rs = y_ctb * pic_w + x_ctb;
    uint32_t slice_rs = sc->sh->slice_segment_address;

    memset(&info, 0, sizeof(info));

    if (x_ctb > 0) {
        int left_in_slice = addr_rs > slice_rs;
        if (left_in_slice)
            merge_left =
                heic_cabac_decode_bin(&sc->cabac, &sc->models[HEIC_CTX_SAO_MERGE_FLAG])
                != 0;
    }
    if (y_ctb > 0 && !merge_left) {
        int up_in_slice = addr_rs >= pic_w + slice_rs;
        if (up_in_slice)
            merge_up =
                heic_cabac_decode_bin(&sc->cabac, &sc->models[HEIC_CTX_SAO_MERGE_FLAG])
                != 0;
    }

    if (merge_left)
        info = sc->sao_map[y_ctb * sc->sao_stride + (x_ctb - 1)];
    else if (merge_up)
        info = sc->sao_map[(y_ctb - 1) * sc->sao_stride + x_ctb];
    else {
        int is_mono = sc->sps->chroma_format_idc == 0;
        int n_chroma = is_mono ? 1 : 3;
        uint8_t sao_type_luma = 0, sao_type_chroma = 0, eo_chroma = 0;
        int c_idx;
        for (c_idx = 0; c_idx < n_chroma; c_idx++) {
            int should = (sc->sh->slice_sao_luma_flag && c_idx == 0)
                         || (sc->sh->slice_sao_chroma_flag && c_idx > 0);
            uint8_t type_idx;
            if (!should) continue;
            if (c_idx == 0) {
                sao_type_luma = decode_sao_type_idx(sc);
                type_idx = sao_type_luma;
            } else if (c_idx == 1) {
                sao_type_chroma = decode_sao_type_idx(sc);
                type_idx = sao_type_chroma;
            } else {
                type_idx = sao_type_chroma;
            }
            info.sao_type_idx[c_idx] = type_idx;
            if (type_idx != 0) {
                int bd = c_idx == 0 ? bit_depth_y(sc->sps) : bit_depth_c(sc->sps);
                uint32_t c_max = (1u << (((bd < 10 ? bd : 10) - 5))) - 1;
                int scale = 1 << (bd > 10 ? bd - 10 : 0);
                uint32_t offs[4];
                int e;
                for (e = 0; e < 4; e++) offs[e] = decode_tu_bypass(sc, c_max);
                if (type_idx == 1) {
                    for (e = 0; e < 4; e++) {
                        if (offs[e]) {
                            int sign = heic_cabac_decode_bypass(&sc->cabac);
                            int16_t val = (int16_t)((int)offs[e] * scale);
                            info.sao_offset_val[c_idx][e] = sign ? (int16_t)(-val) : val;
                        }
                    }
                    info.sao_band_position[c_idx] =
                        (uint8_t)heic_cabac_decode_bypass_bits(&sc->cabac, 5);
                } else {
                    for (e = 0; e < 4; e++)
                        info.sao_offset_val[c_idx][e] =
                            (int16_t)((int)offs[e] * scale);
                    if (c_idx <= 1) {
                        uint8_t eo = (uint8_t)heic_cabac_decode_bypass_bits(&sc->cabac, 2);
                        if (c_idx == 0) info.sao_eo_class[0] = eo;
                        else {
                            eo_chroma = eo;
                            info.sao_eo_class[1] = eo;
                        }
                    } else {
                        info.sao_eo_class[2] = eo_chroma;
                    }
                }
            }
        }
    }
    sc->sao_map[y_ctb * sc->sao_stride + x_ctb] = info;
    return 0;
}

static int decode_ctu(heic_slice_ctx *sc, uint32_t x_ctb, uint32_t y_ctb)
{
    if (sc->pps->cu_qp_delta_enabled_flag) {
        sc->is_cu_qp_delta_coded = 0;
        sc->cu_qp_delta = 0;
    }
    if (sc->sh->slice_sao_luma_flag || sc->sh->slice_sao_chroma_flag) {
        if (decode_sao(sc, x_ctb, y_ctb) != 0) return -1;
    }
    return decode_cqt(sc, x_ctb, y_ctb, sc->sps->log2_ctb_size, 0);
}

static int slice_ctx_init(heic_slice_ctx *sc, heic_ctx *ctx, const heic_sps *sps,
                          const heic_pps *pps, const heic_slice_header *sh,
                          const uint8_t *data, size_t len, heic_frame *frame)
{
    uint32_t min_cb = 1u << sps->log2_min_cb_size;
    uint32_t min_pu = min_pu_size(sps);
    uint32_t min_tb = 1u << sps->log2_min_tb_size;
    uint32_t ct_w, ct_h, pu_w, pu_h, qp_w, qp_h;
    size_t ct_n, pu_n, qp_n, sao_n;

    memset(sc, 0, sizeof(*sc));
    sc->hctx = ctx;
    sc->sps = sps;
    sc->pps = pps;
    sc->sh = sh;
    sc->frame = frame;
    sc->current_qg_x = -1;
    sc->current_qg_y = -1;
    sc->current_qpy = sh->slice_qp_y;
    sc->last_qpy_in_prev_qg = sh->slice_qp_y;
    sc->qp_y = sh->slice_qp_y;

    if (heic_cabac_new(&sc->cabac, data, len) != 0) return -1;
    heic_cabac_init_contexts(sc->models, sh->slice_type, sh->cabac_init_flag,
                             sh->slice_qp_y);

    ct_w = (sps->pic_width_in_luma_samples + min_cb - 1) / min_cb;
    ct_h = (sps->pic_height_in_luma_samples + min_cb - 1) / min_cb;
    sc->ct_depth_stride = ct_w;
    ct_n = (size_t)ct_w * ct_h;
    sc->ct_depth_n = ct_n;
    sc->ct_depth_map = (uint8_t *)heic_zalloc(ctx, ct_n);
    if (!sc->ct_depth_map) return -1;
    memset(sc->ct_depth_map, 0xFF, ct_n);

    pu_w = (sps->pic_width_in_luma_samples + min_pu - 1) / min_pu;
    pu_h = (sps->pic_height_in_luma_samples + min_pu - 1) / min_pu;
    sc->intra_mode_stride = pu_w;
    pu_n = (size_t)pu_w * pu_h;
    sc->intra_mode_n = pu_n;
    sc->intra_mode_map = (uint8_t *)heic_zalloc(ctx, pu_n);
    sc->intra_chroma_mode_map = (uint8_t *)heic_zalloc(ctx, pu_n);
    if (!sc->intra_mode_map || !sc->intra_chroma_mode_map) return -1;
    memset(sc->intra_mode_map, 1, pu_n); /* DC default */
    memset(sc->intra_chroma_mode_map, 1, pu_n);

    qp_w = (sps->pic_width_in_luma_samples + min_tb - 1) / min_tb;
    qp_h = (sps->pic_height_in_luma_samples + min_tb - 1) / min_tb;
    sc->qp_map_stride = qp_w;
    qp_n = (size_t)qp_w * qp_h;
    sc->qp_map_n = qp_n;
    sc->qp_map = (int8_t *)heic_zalloc(ctx, qp_n);
    if (!sc->qp_map) return -1;
    {
        size_t i;
        for (i = 0; i < qp_n; i++) sc->qp_map[i] = (int8_t)sh->slice_qp_y;
    }

    sc->sao_stride = sps->pic_width_in_ctbs;
    sao_n = (size_t)sps->pic_width_in_ctbs * sps->pic_height_in_ctbs;
    sc->sao_map = (heic_sao_info *)heic_zalloc(ctx, sao_n * sizeof(heic_sao_info));
    if (!sc->sao_map) return -1;

    sc->deblock_stride = (sps->pic_width_in_luma_samples + 3) / 4;
    {
        uint32_t db_h = (sps->pic_height_in_luma_samples + 3) / 4;
        size_t db_n = (size_t)sc->deblock_stride * db_h;
        sc->deblock_n = (uint32_t)db_n;
        sc->deblock_flags = (uint8_t *)heic_zalloc(ctx, db_n);
        sc->deblock_qp = (int8_t *)heic_zalloc(ctx, db_n);
        if (!sc->deblock_flags || !sc->deblock_qp) return -1;
        {
            size_t i;
            for (i = 0; i < db_n; i++) sc->deblock_qp[i] = (int8_t)sh->slice_qp_y;
        }
    }
    sc->residual_buf =
        (int16_t *)heic_zalloc(ctx, (size_t)HEIC_MAX_COEFF * sizeof(int16_t));
    sc->coeff = (heic_coeff_buf *)heic_zalloc(ctx, sizeof(heic_coeff_buf));
    if (!sc->residual_buf || !sc->coeff) return -1;
    return 0;
}

static void slice_ctx_free(heic_slice_ctx *sc)
{
    if (!sc || !sc->hctx) return;
    heic_free_buf(sc->hctx, sc->ct_depth_map);
    heic_free_buf(sc->hctx, sc->intra_mode_map);
    heic_free_buf(sc->hctx, sc->intra_chroma_mode_map);
    heic_free_buf(sc->hctx, sc->qp_map);
    heic_free_buf(sc->hctx, sc->sao_map);
    heic_free_buf(sc->hctx, sc->deblock_flags);
    heic_free_buf(sc->hctx, sc->deblock_qp);
    heic_free_buf(sc->hctx, sc->residual_buf);
    heic_free_buf(sc->hctx, sc->coeff);
    memset(sc, 0, sizeof(*sc));
}

/* Map EBSP offset within slice_data to RBSP offset using EP positions. */
static uint32_t ebsp_to_rbsp(const uint32_t *eps, int n_ep, uint32_t ebsp_off)
{
    int i, count = 0;
    for (i = 0; i < n_ep; i++)
        if (eps[i] < ebsp_off) count++;
        else break;
    return ebsp_off - (uint32_t)count;
}

/* col_bd/row_bd must hold nc+1 / nr+1 entries (caller: arrays of size ≥ 65). */
static int compute_tile_bd(const heic_pps *pps, uint32_t pic_w, uint32_t pic_h,
                           uint32_t *col_bd, uint32_t *row_bd,
                           int *n_cols, int *n_rows)
{
    uint32_t nc = (uint32_t)pps->num_tile_columns_minus1 + 1;
    uint32_t nr = (uint32_t)pps->num_tile_rows_minus1 + 1;
    uint32_t i;
    if (nc == 0 || nr == 0 || nc > 64 || nr > 64) return -1;
    if (pic_w == 0 || pic_h == 0) return -1;
    *n_cols = (int)nc;
    *n_rows = (int)nr;
    if (pps->uniform_spacing_flag) {
        for (i = 0; i <= nc; i++) col_bd[i] = (i * pic_w) / nc;
        for (i = 0; i <= nr; i++) row_bd[i] = (i * pic_h) / nr;
    } else {
        uint32_t pos = 0;
        col_bd[0] = 0;
        for (i = 0; i < nc - 1 && pps->column_width_minus1; i++) {
            pos = pos + (uint32_t)pps->column_width_minus1[i] + 1;
            if (pos > pic_w) pos = pic_w;
            col_bd[i + 1] = pos;
        }
        col_bd[nc] = pic_w;
        pos = 0;
        row_bd[0] = 0;
        for (i = 0; i < nr - 1 && pps->row_height_minus1; i++) {
            pos = pos + (uint32_t)pps->row_height_minus1[i] + 1;
            if (pos > pic_h) pos = pic_h;
            row_bd[i + 1] = pos;
        }
        row_bd[nr] = pic_h;
    }
    return 0;
}

static uint32_t get_tile_id(const uint32_t *col_bd, int n_cols,
                            const uint32_t *row_bd, int n_rows,
                            uint32_t cx, uint32_t cy)
{
    int tc = 0, tr = 0, i;
    for (i = 0; i < n_cols; i++)
        if (cx >= col_bd[i] && cx < col_bd[i + 1]) {
            tc = i;
            break;
        }
    for (i = 0; i < n_rows; i++)
        if (cy >= row_bd[i] && cy < row_bd[i + 1]) {
            tr = i;
            break;
        }
    return (uint32_t)tr * (uint32_t)n_cols + (uint32_t)tc;
}

static int build_tile_scan(heic_ctx *ctx, const uint32_t *col_bd, int n_cols,
                           const uint32_t *row_bd, int n_rows,
                           uint32_t **out_xy, int *out_n)
{
    int tr, tc;
    int n = 0, cap = 0;
    uint32_t *scan = NULL;
    for (tr = 0; tr < n_rows; tr++) {
        for (tc = 0; tc < n_cols; tc++) {
            uint32_t cy, cx;
            for (cy = row_bd[tr]; cy < row_bd[tr + 1]; cy++) {
                for (cx = col_bd[tc]; cx < col_bd[tc + 1]; cx++) {
                    if (n + 2 > cap) {
                        int ncap = cap ? cap * 2 : 64;
                        uint32_t *np = (uint32_t *)heic_realloc_buf(
                            ctx, scan, (size_t)cap * sizeof(uint32_t),
                            (size_t)ncap * sizeof(uint32_t));
                        if (!np) {
                            heic_free_buf(ctx, scan);
                            return -1;
                        }
                        scan = np;
                        cap = ncap;
                    }
                    scan[n++] = cx;
                    scan[n++] = cy;
                }
            }
        }
    }
    *out_xy = scan;
    *out_n = n / 2;
    return 0;
}

/* Heavy scratch lives on the heap: under ASan a ~20KB stack frame here plus
 * nested iden/grid/HEVC decode blows the default stack during fuzz REDUCE. */
typedef struct {
    heic_slice_ctx   sc;
    uint32_t         col_bd[65];
    uint32_t         row_bd[65];
    uint32_t         entry_cum[4096];
    heic_ctx_model   wpp_saved[HEIC_NUM_CONTEXTS];
} heic_slice_work;

int heic_hevc_decode_slice_i(heic_ctx *ctx, const heic_sps *sps,
                             const heic_pps *pps, const heic_slice_header *sh,
                             const uint8_t *data, size_t len,
                             const uint32_t *ep_positions, int n_ep,
                             heic_frame *out, const heic_abort *ab)
{
    heic_slice_work *work;
    heic_slice_ctx *sc;
    uint32_t ctb = 0, total, start, pic_w, pic_h, ctb_sz;
    int tiles, wpp;
    int n_cols = 1, n_rows = 1;
    uint32_t *tile_scan = NULL;
    int tile_scan_n = 0, tile_scan_pos = 0;
    int n_entry = 0, entry_idx = 0;
    int wpp_have_saved = 0;
    int i;

    if (!ctx || !sps || !pps || !sh || !data || !out) return -1;
    if (sh->slice_type != HEIC_SLICE_I) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "only I-slice CTU path implemented");
        return -1;
    }
    total = sps->pic_size_in_ctbs;
    start = sh->slice_segment_address;
    if (total == 0 || start >= total) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "slice_segment_address out of range");
        return -1;
    }

    work = (heic_slice_work *)heic_zalloc(ctx, sizeof(*work));
    if (!work) return -1;
    sc = &work->sc;

    if (slice_ctx_init(sc, ctx, sps, pps, sh, data, len, out) != 0) {
        slice_ctx_free(sc);
        heic_free_buf(ctx, work);
        return -1;
    }

    pic_w = sps->pic_width_in_ctbs;
    pic_h = sps->pic_height_in_ctbs;
    ctb_sz = ctb_size_px(sps);
    tiles = pps->tiles_enabled_flag;
    wpp = pps->entropy_coding_sync_enabled_flag;

    if (tiles) {
        if (compute_tile_bd(pps, pic_w, pic_h, work->col_bd, work->row_bd, &n_cols,
                            &n_rows) != 0) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "invalid tile grid");
            slice_ctx_free(sc);
            heic_free_buf(ctx, work);
            return -1;
        }
        if (build_tile_scan(ctx, work->col_bd, n_cols, work->row_bd, n_rows, &tile_scan,
                            &tile_scan_n) != 0) {
            slice_ctx_free(sc);
            heic_free_buf(ctx, work);
            return -1;
        }
        /* find start in tile scan */
        for (i = 0; i < tile_scan_n; i++) {
            if (tile_scan[i * 2] == start % pic_w &&
                tile_scan[i * 2 + 1] == start / pic_w) {
                tile_scan_pos = i;
                break;
            }
        }
        sc->ctb_x = tile_scan[tile_scan_pos * 2];
        sc->ctb_y = tile_scan[tile_scan_pos * 2 + 1];
    } else {
        sc->ctb_y = start / pic_w;
        sc->ctb_x = start % pic_w;
    }

    /* cumulative entry offsets (EBSP, relative to slice_data start) */
    if (sh->num_entry_point_offsets > 0 && sh->entry_point_offsets) {
        uint32_t cum = 0;
        n_entry = (int)sh->num_entry_point_offsets;
        if (n_entry > 4096) n_entry = 4096;
        for (i = 0; i < n_entry; i++) {
            cum += sh->entry_point_offsets[i];
            work->entry_cum[i] = cum;
        }
    }

    for (;;) {
        uint32_t x = sc->ctb_x * ctb_sz;
        uint32_t y = sc->ctb_y * ctb_sz;
        int end_flag;
        uint32_t prev_x = sc->ctb_x, prev_y = sc->ctb_y;

        if (heic_abort_check(ab)) {
            heic_free_buf(ctx, tile_scan);
            slice_ctx_free(sc);
            heic_free_buf(ctx, work);
            return -1;
        }
        if (heic_cabac_overread(&sc->cabac)) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "CABAC overread during CTU decode");
            heic_free_buf(ctx, tile_scan);
            slice_ctx_free(sc);
            heic_free_buf(ctx, work);
            return -1;
        }
        if (decode_ctu(sc, x, y) != 0 || sc->cabac.error) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "CTU decode failed at (%u,%u) ctu=%u",
                       sc->ctb_x, sc->ctb_y, ctb);
            heic_free_buf(ctx, tile_scan);
            slice_ctx_free(sc);
            heic_free_buf(ctx, work);
            return -1;
        }
        ctb++;

        /* WPP: save contexts after CTB column 1 (for next row restore) */
        if (wpp && sc->ctb_x == 1 && sc->ctb_y + 1 < pic_h) {
            memcpy(work->wpp_saved, sc->models, sizeof(work->wpp_saved));
            wpp_have_saved = 1;
        }

        end_flag = heic_cabac_decode_terminate(&sc->cabac);
        if (end_flag) break;

        /* advance CTB */
        if (tiles) {
            tile_scan_pos++;
            if (tile_scan_pos >= tile_scan_n) break;
            sc->ctb_x = tile_scan[tile_scan_pos * 2];
            sc->ctb_y = tile_scan[tile_scan_pos * 2 + 1];
            {
                uint32_t pt =
                    get_tile_id(work->col_bd, n_cols, work->row_bd, n_rows, prev_x, prev_y);
                uint32_t ct = get_tile_id(work->col_bd, n_cols, work->row_bd, n_rows,
                                          sc->ctb_x, sc->ctb_y);
                if (ct != pt) {
                    (void)heic_cabac_decode_terminate(&sc->cabac);
                    if (entry_idx < n_entry) {
                        uint32_t ebsp = work->entry_cum[entry_idx];
                        uint32_t rbsp = ep_positions && n_ep > 0
                                           ? ebsp_to_rbsp(ep_positions, n_ep, ebsp)
                                           : ebsp;
                        heic_cabac_seek(&sc->cabac, rbsp);
                        heic_cabac_reinit(&sc->cabac);
                        entry_idx++;
                    }
                    heic_cabac_init_contexts(sc->models, sh->slice_type, sh->cabac_init_flag,
                                             sh->slice_qp_y);
                    sc->current_qpy = sh->slice_qp_y;
                    sc->last_qpy_in_prev_qg = sh->slice_qp_y;
                    sc->current_qg_x = -1;
                    sc->current_qg_y = -1;
                    sc->is_cu_qp_delta_coded = 0;
                    sc->cu_qp_delta = 0;
                }
            }
        } else {
            sc->ctb_x++;
            if (sc->ctb_x >= pic_w) {
                sc->ctb_x = 0;
                sc->ctb_y++;
                /* WPP: new row — restore contexts from prev row col1 + seek entry */
                if (wpp && sc->ctb_y < pic_h && pic_w > 1) {
                    (void)heic_cabac_decode_terminate(&sc->cabac); /* end_of_subset */
                    if (wpp_have_saved)
                        memcpy(sc->models, work->wpp_saved, sizeof(sc->models));
                    if (entry_idx < n_entry) {
                        uint32_t ebsp = work->entry_cum[entry_idx];
                        uint32_t rbsp = ep_positions && n_ep > 0
                                           ? ebsp_to_rbsp(ep_positions, n_ep, ebsp)
                                           : ebsp;
                        heic_cabac_seek(&sc->cabac, rbsp);
                        heic_cabac_reinit(&sc->cabac);
                        entry_idx++;
                    }
                }
            }
            if (sc->ctb_y >= pic_h) break;
        }
    }

    heic_error(ctx, HEIC_SEVERITY_INFO, "I-slice decoded %u CTUs", (unsigned)ctb);

    /* Loop filters: deblock then SAO (H.265 8.7). */
    if (!sh->slice_deblocking_filter_disabled_flag && sc->deblock_flags &&
        sc->deblock_qp) {
        int beta = (int)sh->slice_beta_offset_div2 * 2;
        int tc = (int)sh->slice_tc_offset_div2 * 2;
        int cb_off = (int)pps->pps_cb_qp_offset + (int)sh->slice_cb_qp_offset;
        int cr_off = (int)pps->pps_cr_qp_offset + (int)sh->slice_cr_qp_offset;
        heic_apply_deblock(out, sc->deblock_flags, sc->deblock_qp, sc->deblock_stride,
                           beta, tc, cb_off, cr_off);
    }
    if (sps->sample_adaptive_offset_enabled_flag &&
        (sh->slice_sao_luma_flag || sh->slice_sao_chroma_flag) && sc->sao_map) {
        heic_apply_sao(ctx, out, sc->sao_map, sps->pic_width_in_ctbs,
                       sps->pic_height_in_ctbs, ctb_sz);
    }

    heic_free_buf(ctx, tile_scan);
    slice_ctx_free(sc);
    heic_free_buf(ctx, work);
    return 0;
}
