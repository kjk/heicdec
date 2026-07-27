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
    int is_cu_chroma_qp_offset_coded;
    int cu_qp_offset_cb, cu_qp_offset_cr;
    int cu_transquant_bypass;
    uint32_t cu_base_x, cu_base_y;
    uint8_t cu_log2_size;

    uint8_t *ct_depth_map;
    uint32_t ct_depth_stride;
    size_t   ct_depth_n;
    uint8_t *intra_mode_map;
    uint8_t *intra_chroma_mode_map;
    uint8_t *pred_mode_map;
    heic_pb_motion *mv_info;
    uint32_t intra_mode_stride;
    size_t   intra_mode_n;
    const heic_frame *refs[2][HEIC_MAX_REF_PICS];
    int n_refs[2];
    int cu_pred_mode;
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
    uint8_t *cbf_map;
    uint8_t *pcm_map;
    int has_filter_exclusions;
    int8_t  *deblock_qp;
    uint32_t deblock_stride;
    uint32_t deblock_n;

    int16_t *residual_buf; /* HEIC_MAX_COEFF, heap (keep CTU frame small for ASan) */
    int32_t *luma_residual; /* current luma TU for RExt cross-component prediction */
    uint8_t luma_residual_log2;
    heic_coeff_buf *coeff; /* residual decode scratch (2KB; not on recursive stack) */
    int32_t *mc_scratch; /* max 64x(64+8), shared by luma/chroma MC */
    int16_t *mc_internal; /* two lists of internal-precision Y + Cb + Cr */
    uint8_t stat_coeff[4]; /* RExt persistent Rice adaptation state */
} heic_slice_ctx;

enum {
    HEIC_PART_2NX2N = 0,
    HEIC_PART_2NXN = 1,
    HEIC_PART_NX2N = 2,
    HEIC_PART_NXN = 3,
    HEIC_PART_2NXNU = 4,
    HEIC_PART_2NXND = 5,
    HEIC_PART_NLX2N = 6,
    HEIC_PART_NRX2N = 7
};

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

static int predict_intra_block(heic_slice_ctx *sc, uint32_t x, uint32_t y,
                               uint8_t log2_size, uint8_t mode, uint8_t c_idx,
                               int strong_intra_smoothing)
{
    const uint8_t *pred_mode_map =
        sc->pps->constrained_intra_pred_flag ? sc->pred_mode_map : NULL;
    return heic_predict_intra(
        sc->frame, x, y, log2_size, mode, c_idx, strong_intra_smoothing,
        sc->sh->slice_address, sc->sps->pic_width_in_ctbs,
        ctb_size_px(sc->sps), pred_mode_map, sc->intra_mode_stride,
        sc->intra_mode_n, min_pu_size(sc->sps));
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
    uint32_t ctb, addr;
    if (x < 0 || y < 0) return 0;
    if ((uint32_t)x >= sc->sps->pic_width_in_luma_samples) return 0;
    if ((uint32_t)y >= sc->sps->pic_height_in_luma_samples) return 0;
    ctb = ctb_size_px(sc->sps);
    addr = ((uint32_t)y / ctb) * sc->sps->pic_width_in_ctbs
         + (uint32_t)x / ctb;
    if (addr < sc->sh->slice_address) return 0;
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
    size_t idx;
    const uint8_t *map = chroma ? sc->intra_chroma_mode_map : sc->intra_mode_map;
    if (!neighbor_avail(sc, (int32_t)x, (int32_t)y)) return 1;
    idx = (size_t)(y / mpu) * sc->intra_mode_stride + (x / mpu);
    if (idx >= sc->intra_mode_n) return 1; /* DC */
    return map[idx];
}

static uint8_t get_pred_mode(const heic_slice_ctx *sc, int32_t x, int32_t y)
{
    uint32_t mpu;
    size_t idx;
    if (!neighbor_avail(sc, x, y))
        return HEIC_PRED_UNAVAILABLE;
    mpu = min_pu_size(sc->sps);
    idx = (size_t)((uint32_t)y / mpu) * sc->intra_mode_stride +
          (uint32_t)x / mpu;
    return idx < sc->intra_mode_n ? sc->pred_mode_map[idx]
                                  : HEIC_PRED_UNAVAILABLE;
}

static void store_pred_mode(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                            uint32_t w, uint32_t h, uint8_t mode)
{
    uint32_t mpu = min_pu_size(sc->sps);
    uint32_t sx = x0 / mpu, sy = y0 / mpu;
    uint32_t nx = (w + mpu - 1) / mpu, ny = (h + mpu - 1) / mpu;
    uint32_t dx, dy;
    for (dy = 0; dy < ny; dy++)
        for (dx = 0; dx < nx; dx++) {
            size_t idx = (size_t)(sy + dy) * sc->intra_mode_stride + sx + dx;
            if (idx < sc->intra_mode_n) sc->pred_mode_map[idx] = mode;
        }
}

static heic_pb_motion get_motion(const heic_slice_ctx *sc, int32_t x, int32_t y)
{
    heic_pb_motion none;
    uint32_t mpu;
    size_t idx;
    memset(&none, 0, sizeof(none));
    none.ref_idx[0] = none.ref_idx[1] = -1;
    if (get_pred_mode(sc, x, y) != HEIC_PRED_INTER &&
        get_pred_mode(sc, x, y) != HEIC_PRED_SKIP)
        return none;
    mpu = min_pu_size(sc->sps);
    idx = (size_t)((uint32_t)y / mpu) * sc->intra_mode_stride +
          (uint32_t)x / mpu;
    return idx < sc->intra_mode_n ? sc->mv_info[idx] : none;
}

static void store_motion(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                         uint32_t w, uint32_t h, heic_pb_motion motion)
{
    uint32_t mpu = min_pu_size(sc->sps);
    uint32_t sx = x0 / mpu, sy = y0 / mpu;
    uint32_t nx = (w + mpu - 1) / mpu, ny = (h + mpu - 1) / mpu;
    uint32_t dx, dy;
    for (dy = 0; dy < ny; dy++)
        for (dx = 0; dx < nx; dx++) {
            size_t idx = (size_t)(sy + dy) * sc->intra_mode_stride + sx + dx;
            if (idx < sc->intra_mode_n) sc->mv_info[idx] = motion;
        }
}

static void store_cbf(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                      uint32_t size, int has_coeff)
{
    uint32_t sx = x0 / 4, sy = y0 / 4;
    uint32_t n = (size + 3) / 4, dx, dy;
    if (!sc->cbf_map) return;
    for (dy = 0; dy < n; dy++)
        for (dx = 0; dx < n; dx++) {
            size_t idx =
                (size_t)(sy + dy) * sc->deblock_stride + sx + dx;
            if (idx < sc->deblock_n)
                sc->cbf_map[idx] = (uint8_t)(has_coeff != 0);
        }
}

static uint8_t neighbor_intra_left(const heic_slice_ctx *sc, uint32_t x0, uint32_t y0)
{
    if (x0 == 0
        || !neighbor_avail(sc, (int32_t)x0 - 1, (int32_t)y0))
        return 1; /* DC */
    return get_intra_mode(sc, x0 - 1, y0, 0);
}

static uint8_t neighbor_intra_above(const heic_slice_ctx *sc, uint32_t x0, uint32_t y0)
{
    uint32_t ctb = ctb_size_px(sc->sps);
    uint32_t ctb_y0;
    if (y0 == 0
        || !neighbor_avail(sc, (int32_t)x0, (int32_t)y0 - 1))
        return 1;
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
    slice_sx = (sc->sh->slice_address % sc->sps->pic_width_in_ctbs)
               * ctb_size_px(sc->sps);
    slice_sy = (sc->sh->slice_address / sc->sps->pic_width_in_ctbs)
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
    qpi_cb = qpy + sc->pps->pps_cb_qp_offset + sc->sh->slice_cb_qp_offset
             + sc->cu_qp_offset_cb;
    qpi_cr = qpy + sc->pps->pps_cr_qp_offset + sc->sh->slice_cr_qp_offset
             + sc->cu_qp_offset_cr;
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

typedef struct {
    int merge_flag;
    uint8_t merge_idx;
    uint8_t inter_pred_idc; /* 1=L0, 2=L1, 3=bi */
    int8_t ref_idx[2];
    int16_t mvd[2][2];
    uint8_t mvp_flag[2];
} heic_pb_coding;

typedef struct {
    uint32_t x, y, w, h;
} heic_pu;

static int decode_cu_skip(heic_slice_ctx *sc, uint32_t x0, uint32_t y0)
{
    int inc = 0;
    if (get_pred_mode(sc, (int32_t)x0 - 1, (int32_t)y0) == HEIC_PRED_SKIP)
        inc++;
    if (get_pred_mode(sc, (int32_t)x0, (int32_t)y0 - 1) == HEIC_PRED_SKIP)
        inc++;
    return heic_cabac_decode_bin(&sc->cabac,
                                 &sc->models[HEIC_CTX_CU_SKIP_FLAG + inc])
           != 0;
}

static int decode_part_mode_inter(heic_slice_ctx *sc, uint8_t log2_cb)
{
    int b0 = heic_cabac_decode_bin(&sc->cabac,
                                   &sc->models[HEIC_CTX_PART_MODE]);
    if (b0) return HEIC_PART_2NX2N;
    if (log2_cb == sc->sps->log2_min_cb_size) {
        int b1 = heic_cabac_decode_bin(&sc->cabac,
                                       &sc->models[HEIC_CTX_PART_MODE + 1]);
        if (b1) return HEIC_PART_2NXN;
        if (log2_cb > 3) {
            int b2 = heic_cabac_decode_bin(&sc->cabac,
                                           &sc->models[HEIC_CTX_PART_MODE + 2]);
            return b2 ? HEIC_PART_NX2N : HEIC_PART_NXN;
        }
        return HEIC_PART_NX2N;
    }
    if (sc->sps->amp_enabled_flag) {
        int b1 = heic_cabac_decode_bin(&sc->cabac,
                                       &sc->models[HEIC_CTX_PART_MODE + 1]);
        int b3 = heic_cabac_decode_bin(&sc->cabac,
                                       &sc->models[HEIC_CTX_PART_MODE + 3]);
        if (b1) {
            if (b3) return HEIC_PART_2NXN;
            return heic_cabac_decode_bypass(&sc->cabac)
                       ? HEIC_PART_2NXND
                       : HEIC_PART_2NXNU;
        }
        if (b3) return HEIC_PART_NX2N;
        return heic_cabac_decode_bypass(&sc->cabac)
                   ? HEIC_PART_NRX2N
                   : HEIC_PART_NLX2N;
    }
    return heic_cabac_decode_bin(&sc->cabac,
                                 &sc->models[HEIC_CTX_PART_MODE + 1])
               ? HEIC_PART_2NXN
               : HEIC_PART_NX2N;
}

static int partition_to_pus(int mode, uint32_t x, uint32_t y, uint32_t n,
                            heic_pu pu[4])
{
    uint32_t h = n / 2, q = n / 4;
    switch (mode) {
    case HEIC_PART_2NX2N:
        pu[0] = (heic_pu){x, y, n, n}; return 1;
    case HEIC_PART_2NXN:
        pu[0] = (heic_pu){x, y, n, h};
        pu[1] = (heic_pu){x, y + h, n, h}; return 2;
    case HEIC_PART_NX2N:
        pu[0] = (heic_pu){x, y, h, n};
        pu[1] = (heic_pu){x + h, y, h, n}; return 2;
    case HEIC_PART_NXN:
        pu[0] = (heic_pu){x, y, h, h};
        pu[1] = (heic_pu){x + h, y, h, h};
        pu[2] = (heic_pu){x, y + h, h, h};
        pu[3] = (heic_pu){x + h, y + h, h, h}; return 4;
    case HEIC_PART_2NXNU:
        pu[0] = (heic_pu){x, y, n, q};
        pu[1] = (heic_pu){x, y + q, n, n - q}; return 2;
    case HEIC_PART_2NXND:
        pu[0] = (heic_pu){x, y, n, n - q};
        pu[1] = (heic_pu){x, y + n - q, n, q}; return 2;
    case HEIC_PART_NLX2N:
        pu[0] = (heic_pu){x, y, q, n};
        pu[1] = (heic_pu){x + q, y, n - q, n}; return 2;
    case HEIC_PART_NRX2N:
        pu[0] = (heic_pu){x, y, n - q, n};
        pu[1] = (heic_pu){x + n - q, y, q, n}; return 2;
    default:
        return 0;
    }
}

static uint8_t decode_merge_idx(heic_slice_ctx *sc)
{
    uint8_t idx = 0;
    if (sc->sh->max_num_merge_cand <= 1) return 0;
    if (!heic_cabac_decode_bin(&sc->cabac, &sc->models[HEIC_CTX_MERGE_IDX]))
        return 0;
    idx = 1;
    while (idx < sc->sh->max_num_merge_cand - 1) {
        if (!heic_cabac_decode_bypass(&sc->cabac)) break;
        idx++;
    }
    return idx;
}

static int16_t decode_mvd_component(heic_slice_ctx *sc, int gt0, int gt1)
{
    int v, sign;
    if (!gt0) return 0;
    v = gt1 ? (int)heic_cabac_decode_egk(&sc->cabac, 1) + 2 : 1;
    if (v > INT16_MAX) {
        sc->cabac.error = 1;
        return 0;
    }
    sign = heic_cabac_decode_bypass(&sc->cabac);
    return (int16_t)(sign ? -v : v);
}

static void decode_mvd(heic_slice_ctx *sc, int16_t *mx, int16_t *my)
{
    int gx = heic_cabac_decode_bin(
        &sc->cabac, &sc->models[HEIC_CTX_ABS_MVD_GREATER0_FLAG]);
    int gy = heic_cabac_decode_bin(
        &sc->cabac, &sc->models[HEIC_CTX_ABS_MVD_GREATER0_FLAG]);
    int g1x = gx ? heic_cabac_decode_bin(
                       &sc->cabac,
                       &sc->models[HEIC_CTX_ABS_MVD_GREATER0_FLAG + 1])
                 : 0;
    int g1y = gy ? heic_cabac_decode_bin(
                       &sc->cabac,
                       &sc->models[HEIC_CTX_ABS_MVD_GREATER0_FLAG + 1])
                 : 0;
    *mx = decode_mvd_component(sc, gx, g1x);
    *my = decode_mvd_component(sc, gy, g1y);
}

static int8_t decode_ref_idx(heic_slice_ctx *sc, int n_active)
{
    int c_max, first, second, idx;
    if (n_active <= 1) return 0;
    c_max = n_active - 1;
    first = heic_cabac_decode_bin(
        &sc->cabac, &sc->models[HEIC_CTX_REF_IDX]);
    if (!first) return 0;
    if (c_max == 1) return 1;
    second = heic_cabac_decode_bin(
        &sc->cabac, &sc->models[HEIC_CTX_REF_IDX + 1]);
    if (!second) return 1;
    idx = 2;
    while (idx < c_max && heic_cabac_decode_bypass(&sc->cabac)) idx++;
    return (int8_t)idx;
}

static uint8_t decode_inter_pred_idc(heic_slice_ctx *sc, uint8_t ct_depth,
                                     uint32_t w, uint32_t h)
{
    int bin;
    if (w + h == 12) {
        bin = heic_cabac_decode_bin(
            &sc->cabac, &sc->models[HEIC_CTX_INTER_PRED_IDC + 4]);
        return (uint8_t)(bin ? 2 : 1);
    }
    bin = heic_cabac_decode_bin(
        &sc->cabac,
        &sc->models[HEIC_CTX_INTER_PRED_IDC + (ct_depth < 3 ? ct_depth : 3)]);
    if (bin) return 3;
    bin = heic_cabac_decode_bin(
        &sc->cabac, &sc->models[HEIC_CTX_INTER_PRED_IDC + 4]);
    return (uint8_t)(bin ? 2 : 1);
}

static heic_pb_coding decode_inter_pu(heic_slice_ctx *sc, int skip,
                                      uint8_t ct_depth, uint32_t w, uint32_t h)
{
    heic_pb_coding c;
    int uses_l0, uses_l1;
    memset(&c, 0, sizeof(c));
    c.ref_idx[0] = c.ref_idx[1] = -1;
    if (skip) {
        c.merge_flag = 1;
        c.merge_idx = decode_merge_idx(sc);
        return c;
    }
    c.merge_flag = heic_cabac_decode_bin(
                       &sc->cabac, &sc->models[HEIC_CTX_MERGE_FLAG])
                   != 0;
    if (c.merge_flag) {
        c.merge_idx = decode_merge_idx(sc);
        return c;
    }
    c.inter_pred_idc = sc->sh->slice_type == HEIC_SLICE_B
        ? decode_inter_pred_idc(sc, ct_depth, w, h) : 1;
    uses_l0 = c.inter_pred_idc == 1 || c.inter_pred_idc == 3;
    uses_l1 = c.inter_pred_idc == 2 || c.inter_pred_idc == 3;
    if (uses_l0) {
        c.ref_idx[0] = decode_ref_idx(sc, sc->sh->num_ref_idx_l0_active);
        decode_mvd(sc, &c.mvd[0][0], &c.mvd[0][1]);
        c.mvp_flag[0] = (uint8_t)heic_cabac_decode_bin(
            &sc->cabac, &sc->models[HEIC_CTX_MVP_LX_FLAG]);
    }
    if (uses_l1) {
        c.ref_idx[1] = decode_ref_idx(sc, sc->sh->num_ref_idx_l1_active);
        if (!sc->sh->mvd_l1_zero_flag || c.inter_pred_idc != 3)
            decode_mvd(sc, &c.mvd[1][0], &c.mvd[1][1]);
        c.mvp_flag[1] = (uint8_t)heic_cabac_decode_bin(
            &sc->cabac, &sc->models[HEIC_CTX_MVP_LX_FLAG]);
    }
    return c;
}

static int same_ref_picture(const heic_slice_ctx *sc,
                            int list_a, int ref_a, int list_b, int ref_b)
{
    const heic_frame *a, *b;
    if (list_a < 0 || list_a > 1 || list_b < 0 || list_b > 1 ||
        ref_a < 0 || ref_a >= sc->n_refs[list_a] ||
        ref_b < 0 || ref_b >= sc->n_refs[list_b])
        return 0;
    a = sc->refs[list_a][ref_a];
    b = sc->refs[list_b][ref_b];
    if (!a || !b) return 0;
    return a == b || (a->poc_valid && b->poc_valid && a->poc == b->poc);
}

static int motion_eq(const heic_slice_ctx *sc,
                     heic_pb_motion a, heic_pb_motion b)
{
    int list;
    if (a.pred_flag[0] != b.pred_flag[0] ||
        a.pred_flag[1] != b.pred_flag[1])
        return 0;
    for (list = 0; list < 2; list++) {
        if (!a.pred_flag[list]) continue;
        if (!same_ref_picture(sc, list, a.ref_idx[list],
                              list, b.ref_idx[list]) ||
            a.mv[list].x != b.mv[list].x ||
            a.mv[list].y != b.mv[list].y)
            return 0;
    }
    return 1;
}

static int same_merge_region(const heic_slice_ctx *sc, uint32_t x, uint32_t y,
                             int32_t nx, int32_t ny)
{
    int shift = sc->pps->log2_parallel_merge_level_minus2 + 2;
    if (nx < 0 || ny < 0) return 0;
    return (x >> shift) == ((uint32_t)nx >> shift) &&
           (y >> shift) == ((uint32_t)ny >> shift);
}

static int inter_at(const heic_slice_ctx *sc, int32_t x, int32_t y)
{
    uint8_t p = get_pred_mode(sc, x, y);
    return p == HEIC_PRED_INTER || p == HEIC_PRED_SKIP;
}

static heic_pb_motion zero_motion(void)
{
    heic_pb_motion m;
    memset(&m, 0, sizeof(m));
    m.pred_flag[0] = 1;
    m.ref_idx[0] = 0;
    m.ref_idx[1] = -1;
    return m;
}

static heic_pb_motion zero_merge_motion(const heic_slice_ctx *sc, int index)
{
    heic_pb_motion m = zero_motion();
    int n = sc->n_refs[0];
    int ref;
    if (sc->sh->slice_type == HEIC_SLICE_B && sc->n_refs[1] < n)
        n = sc->n_refs[1];
    ref = index < n ? index : 0;
    m.ref_idx[0] = (int8_t)ref;
    if (sc->sh->slice_type == HEIC_SLICE_B && sc->n_refs[1] > 0) {
        m.pred_flag[1] = 1;
        m.ref_idx[1] = (int8_t)ref;
    }
    return m;
}

static int clip_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static heic_mv scale_mv(heic_mv mv, int dist_src, int dist_dst)
{
    heic_mv out = mv;
    int td, tb, tx, scale;
    int32_t vx, vy;
    if (dist_src == 0 || dist_src == dist_dst) return mv;
    td = clip_int(dist_src, -128, 127);
    tb = clip_int(dist_dst, -128, 127);
    tx = (16384 + (td < 0 ? -td : td) / 2) / td;
    scale = clip_int((tb * tx + 32) >> 6, -4096, 4095);
    vx = (int32_t)scale * mv.x;
    vy = (int32_t)scale * mv.y;
    out.x = (int16_t)clip_int(
        (vx + 127 + (vx < 0)) >> 8, INT16_MIN, INT16_MAX);
    out.y = (int16_t)clip_int(
        (vy + 127 + (vy < 0)) >> 8, INT16_MIN, INT16_MAX);
    return out;
}

static int active_ref_is_long_term(const heic_slice_ctx *sc,
                                   int list, int ref_idx)
{
    const heic_st_rps *rps = sc->sh->has_inline_short_term_rps
        ? &sc->sh->inline_short_term_rps
        : &sc->sps->short_term_rps[sc->sh->short_term_ref_pic_set_idx];
    int n_short = 0, n_long = 0, total, entry, i;
    if (list < 0 || list > 1 || ref_idx < 0 ||
        ref_idx >= sc->n_refs[list])
        return 0;
    for (i = 0; i < rps->num_negative_pics; i++)
        n_short += rps->used_by_curr_pic_s0[i] != 0;
    for (i = 0; i < rps->num_positive_pics; i++)
        n_short += rps->used_by_curr_pic_s1[i] != 0;
    for (i = 0;
         i < sc->sh->num_long_term_sps + sc->sh->num_long_term_pics; i++)
        n_long += sc->sh->used_by_curr_pic_lt_flag[i] != 0;
    total = n_short + n_long;
    if (total <= 0) return 0;
    if (list == 0 && sc->sh->ref_pic_list_modification_flag_l0)
        entry = sc->sh->list_entry_l0[ref_idx];
    else if (list == 1 && sc->sh->ref_pic_list_modification_flag_l1)
        entry = sc->sh->list_entry_l1[ref_idx];
    else
        entry = ref_idx % total;
    return entry >= n_short;
}

static int derive_temporal_mv(const heic_slice_ctx *sc, const heic_pu *pu,
                              int target_list, int target_ref_idx, heic_mv *out)
{
    const heic_frame *col;
    uint32_t pos[2][2];
    int n_pos = 0;
    int i;
    int col_list = sc->sh->collocated_from_l0_flag ? 0 : 1;
    if (!sc->sh->slice_temporal_mvp_enabled_flag || !out
        || target_list < 0 || target_list > 1
        || sc->sh->collocated_ref_idx >= sc->n_refs[col_list]
        || target_ref_idx < 0
        || target_ref_idx >= sc->n_refs[target_list])
        return 0;
    col = sc->refs[col_list][sc->sh->collocated_ref_idx];
    if (!col || !col->motion || !col->motion_pred_mode
        || !col->motion_min_pu || !col->poc_valid
        || !sc->refs[target_list][target_ref_idx]
        || !sc->refs[target_list][target_ref_idx]->poc_valid)
        return 0;

    {
        uint32_t br_x = pu->x + pu->w;
        uint32_t br_y = pu->y + pu->h;
        uint32_t ctb = ctb_size_px(sc->sps);
        if (br_x < sc->sps->pic_width_in_luma_samples
            && br_y < sc->sps->pic_height_in_luma_samples
            && br_y / ctb == pu->y / ctb) {
            pos[n_pos][0] = (br_x >> 4) << 4;
            pos[n_pos++][1] = (br_y >> 4) << 4;
        }
        pos[n_pos][0] = ((pu->x + (pu->w >> 1)) >> 4) << 4;
        pos[n_pos++][1] = ((pu->y + (pu->h >> 1)) >> 4) << 4;
    }

    for (i = 0; i < n_pos; i++) {
        uint32_t mx = pos[i][0] / col->motion_min_pu;
        uint32_t my = pos[i][1] / col->motion_min_pu;
        size_t idx = (size_t)my * col->motion_stride + mx;
        heic_pb_motion m;
        int list, ref_idx, col_ref_poc, target_ref_poc;
        int col_long_term, target_long_term;
        int no_backward = 1, lx, ri;
        if (idx >= col->motion_n
            || (col->motion_pred_mode[idx] != HEIC_PRED_INTER
                && col->motion_pred_mode[idx] != HEIC_PRED_SKIP))
            continue;
        m = col->motion[idx];
        for (lx = 0; lx < 2; lx++)
            for (ri = 0; ri < sc->n_refs[lx]; ri++)
                if (sc->refs[lx][ri] && sc->refs[lx][ri]->poc_valid
                    && sc->refs[lx][ri]->poc > sc->frame->poc)
                    no_backward = 0;
        if (!m.pred_flag[0]) list = 1;
        else if (!m.pred_flag[1]) list = 0;
        else if (no_backward) list = target_list;
        else list = sc->sh->collocated_from_l0_flag ? 1 : 0;
        ref_idx = m.ref_idx[list];
        if (ref_idx < 0 || ref_idx >= HEIC_MAX_REF_PICS) continue;
        col_ref_poc = col->ref_poc[list][ref_idx];
        target_ref_poc = sc->refs[target_list][target_ref_idx]->poc;
        col_long_term = col->ref_long_term[list][ref_idx] != 0;
        target_long_term =
            active_ref_is_long_term(sc, target_list, target_ref_idx);
        if (col_long_term != target_long_term) continue;
        if (target_long_term)
            *out = m.mv[list];
        else
            *out = scale_mv(m.mv[list], col->poc - col_ref_poc,
                            sc->frame->poc - target_ref_poc);
        return 1;
    }
    return 0;
}

static heic_pb_motion derive_merge(heic_slice_ctx *sc, const heic_pu *pu,
                                   int part_idx, int part_mode, int wanted)
{
    heic_pb_motion cand[5];
    int count = 0, max = sc->sh->max_num_merge_cand;
    int32_t ax = (int32_t)pu->x - 1;
    int32_t ay = (int32_t)(pu->y + pu->h - 1);
    int32_t b1x = (int32_t)(pu->x + pu->w - 1);
    int32_t b1y = (int32_t)pu->y - 1;
    int32_t b0x = (int32_t)(pu->x + pu->w);
    int32_t b0y = b1y;
    int32_t a0x = ax;
    int32_t a0y = (int32_t)(pu->y + pu->h);
    int32_t b2x = ax;
    int32_t b2y = b1y;
    int a1_avail, b1_avail, a1_idx = -1, b1_idx = -1;
    if (max < 1) max = 1;
    if (max > 5) max = 5;
    a1_avail = inter_at(sc, ax, ay) &&
               !same_merge_region(sc, pu->x, pu->y, ax, ay) &&
               !(part_idx == 1 &&
                 (part_mode == HEIC_PART_NX2N ||
                  part_mode == HEIC_PART_NLX2N ||
                  part_mode == HEIC_PART_NRX2N));
    if (a1_avail && count < max) {
        a1_idx = count;
        cand[count++] = get_motion(sc, ax, ay);
    }

    b1_avail = inter_at(sc, b1x, b1y) &&
               !same_merge_region(sc, pu->x, pu->y, b1x, b1y) &&
               !(part_idx == 1 &&
                 (part_mode == HEIC_PART_2NXN ||
                  part_mode == HEIC_PART_2NXNU ||
                  part_mode == HEIC_PART_2NXND));
    if (b1_avail && count < max) {
        heic_pb_motion m = get_motion(sc, b1x, b1y);
        if (a1_idx >= 0 && motion_eq(sc, cand[a1_idx], m)) {
            b1_idx = a1_idx;
        } else {
            b1_idx = count;
            cand[count++] = m;
        }
    }
    if (inter_at(sc, b0x, b0y) &&
        !same_merge_region(sc, pu->x, pu->y, b0x, b0y) && count < max) {
        heic_pb_motion m = get_motion(sc, b0x, b0y);
        if (b1_idx < 0 || !motion_eq(sc, cand[b1_idx], m))
            cand[count++] = m;
    }
    if (inter_at(sc, a0x, a0y) &&
        !same_merge_region(sc, pu->x, pu->y, a0x, a0y) && count < max) {
        heic_pb_motion m = get_motion(sc, a0x, a0y);
        if (a1_idx < 0 || !motion_eq(sc, cand[a1_idx], m))
            cand[count++] = m;
    }
    if (count < 4 && count < max && inter_at(sc, b2x, b2y) &&
        !same_merge_region(sc, pu->x, pu->y, b2x, b2y)) {
        heic_pb_motion m = get_motion(sc, b2x, b2y);
        int dup = a1_idx >= 0 && motion_eq(sc, cand[a1_idx], m);
        if (!dup && b1_idx >= 0)
            dup = motion_eq(sc, cand[b1_idx], m);
        if (!dup) cand[count++] = m;
    }
    if (count < max) {
        heic_pb_motion m;
        heic_mv tmv;
        int have = 0;
        memset(&m, 0, sizeof(m));
        m.ref_idx[0] = m.ref_idx[1] = -1;
        if (derive_temporal_mv(sc, pu, 0, 0, &tmv)) {
            m.pred_flag[0] = 1;
            m.ref_idx[0] = 0;
            m.mv[0] = tmv;
            have = 1;
        }
        if (sc->sh->slice_type == HEIC_SLICE_B
            && derive_temporal_mv(sc, pu, 1, 0, &tmv)) {
            m.pred_flag[1] = 1;
            m.ref_idx[1] = 0;
            m.mv[1] = tmv;
            have = 1;
        }
        if (have) cand[count++] = m;
    }
    if (sc->sh->slice_type == HEIC_SLICE_B && count > 1 && count < max) {
        static const uint8_t comb[12][2] = {
            {0,1},{1,0},{0,2},{2,0},{1,2},{2,1},
            {0,3},{3,0},{1,3},{3,1},{2,3},{3,2}
        };
        int orig = count, ci;
        for (ci = 0; ci < 12 && count < max; ci++) {
            int a = comb[ci][0], b = comb[ci][1];
            heic_pb_motion m;
            if (a >= orig || b >= orig
                || !cand[a].pred_flag[0] || !cand[b].pred_flag[1])
                continue;
            if (same_ref_picture(sc, 0, cand[a].ref_idx[0],
                                 1, cand[b].ref_idx[1])
                && cand[a].mv[0].x == cand[b].mv[1].x
                && cand[a].mv[0].y == cand[b].mv[1].y)
                continue;
            memset(&m, 0, sizeof(m));
            m.pred_flag[0] = m.pred_flag[1] = 1;
            m.ref_idx[0] = cand[a].ref_idx[0];
            m.ref_idx[1] = cand[b].ref_idx[1];
            m.mv[0] = cand[a].mv[0];
            m.mv[1] = cand[b].mv[1];
            cand[count++] = m;
        }
    }
    {
        int zero_idx = 0;
        while (count < max) {
            cand[count++] = zero_merge_motion(sc, zero_idx++);
        }
    }
    if (wanted < 0 || wanted >= max) wanted = 0;
    return cand[wanted];
}

static int motion_ref_info(const heic_slice_ctx *sc, heic_pb_motion m,
                           int list, int *poc, int *long_term)
{
    int ref = m.ref_idx[list];
    const heic_frame *f;
    if (!m.pred_flag[list] || ref < 0 || ref >= sc->n_refs[list])
        return 0;
    f = sc->refs[list][ref];
    if (!f || !f->poc_valid) return 0;
    *poc = f->poc;
    if (long_term)
        *long_term = active_ref_is_long_term(sc, list, ref);
    return 1;
}

static int unscaled_spatial_mv(const heic_slice_ctx *sc, heic_pb_motion m,
                               int target_list, int target_poc, heic_mv *mv)
{
    int order[2] = {target_list, 1 - target_list};
    int i;
    for (i = 0; i < 2; i++) {
        int list = order[i], poc;
        if (motion_ref_info(sc, m, list, &poc, NULL) && poc == target_poc) {
            *mv = m.mv[list];
            return 1;
        }
    }
    return 0;
}

static int scaled_spatial_mv(const heic_slice_ctx *sc, heic_pb_motion m,
                             int target_list, int target_poc,
                             int target_long_term, heic_mv *mv)
{
    int order[2] = {target_list, 1 - target_list};
    int i;
    for (i = 0; i < 2; i++) {
        int list = order[i], poc, long_term;
        if (motion_ref_info(sc, m, list, &poc, &long_term) &&
            long_term == target_long_term) {
            if (target_long_term)
                *mv = m.mv[list];
            else
                *mv = scale_mv(m.mv[list], sc->frame->poc - poc,
                               sc->frame->poc - target_poc);
            return 1;
        }
    }
    return 0;
}

static void derive_amvp(heic_slice_ctx *sc, const heic_pu *pu, int list,
                        int ref_idx, heic_mv mvp[2])
{
    const int32_t apos[2][2] = {
        {(int32_t)pu->x - 1, (int32_t)(pu->y + pu->h)},
        {(int32_t)pu->x - 1, (int32_t)(pu->y + pu->h - 1)}
    };
    const int32_t bpos[3][2] = {
        {(int32_t)(pu->x + pu->w), (int32_t)pu->y - 1},
        {(int32_t)(pu->x + pu->w - 1), (int32_t)pu->y - 1},
        {(int32_t)pu->x - 1, (int32_t)pu->y - 1}
    };
    heic_mv a = {0, 0}, b = {0, 0};
    const heic_frame *target;
    int target_poc, target_long_term;
    int have_a = 0, have_b = 0, count = 0, i;
    int a0_avail, a1_avail, is_scaled;
    mvp[0].x = mvp[0].y = mvp[1].x = mvp[1].y = 0;
    if (ref_idx < 0 || ref_idx >= sc->n_refs[list]) return;
    target = sc->refs[list][ref_idx];
    if (!target || !target->poc_valid) return;
    target_poc = target->poc;
    target_long_term = active_ref_is_long_term(sc, list, ref_idx);

    a0_avail = inter_at(sc, apos[0][0], apos[0][1]);
    a1_avail = inter_at(sc, apos[1][0], apos[1][1]);
    is_scaled = a0_avail || a1_avail;
    for (i = 0; i < 2 && !have_a; i++)
        if (inter_at(sc, apos[i][0], apos[i][1])) {
            heic_pb_motion m = get_motion(sc, apos[i][0], apos[i][1]);
            have_a = unscaled_spatial_mv(
                sc, m, list, target_poc, &a);
        }
    for (i = 0; i < 2 && !have_a; i++)
        if (inter_at(sc, apos[i][0], apos[i][1])) {
            heic_pb_motion m = get_motion(sc, apos[i][0], apos[i][1]);
            have_a = scaled_spatial_mv(
                sc, m, list, target_poc, target_long_term, &a);
        }
    for (i = 0; i < 3 && !have_b; i++)
        if (inter_at(sc, bpos[i][0], bpos[i][1])) {
            heic_pb_motion m = get_motion(sc, bpos[i][0], bpos[i][1]);
            have_b = unscaled_spatial_mv(
                sc, m, list, target_poc, &b);
        }
    if (!is_scaled && have_b) {
        a = b;
        have_a = 1;
    }
    if (!is_scaled) {
        have_b = 0;
        for (i = 0; i < 3 && !have_b; i++)
            if (inter_at(sc, bpos[i][0], bpos[i][1])) {
                heic_pb_motion m =
                    get_motion(sc, bpos[i][0], bpos[i][1]);
                have_b = scaled_spatial_mv(
                    sc, m, list, target_poc, target_long_term, &b);
            }
    }
    if (have_a) {
        mvp[count++] = a;
        if (have_b && (a.x != b.x || a.y != b.y))
            mvp[count++] = b;
    } else if (have_b) {
        mvp[count++] = b;
    }
    if (count < 2 && !(have_a && have_b
                       && (a.x != b.x || a.y != b.y))) {
        heic_mv tmv;
        if (derive_temporal_mv(sc, pu, list, ref_idx, &tmv)
            && (count == 0
                || tmv.x != mvp[0].x || tmv.y != mvp[0].y))
            mvp[count] = tmv;
    }
}

static heic_pb_motion resolve_motion(heic_slice_ctx *sc, heic_pb_coding coding,
                                     const heic_pu *pu, int part_idx,
                                     int part_mode)
{
    if (coding.merge_flag) {
        heic_pb_motion out =
            derive_merge(sc, pu, part_idx, part_mode, coding.merge_idx);
        if (pu->w + pu->h == 12
            && out.pred_flag[0] && out.pred_flag[1]) {
            out.pred_flag[1] = 0;
            out.ref_idx[1] = -1;
        }
        return out;
    }
    {
        heic_pb_motion out;
        int list;
        memset(&out, 0, sizeof(out));
        out.ref_idx[0] = out.ref_idx[1] = -1;
        for (list = 0; list < 2; list++) {
            heic_mv mvp[2];
            if (coding.ref_idx[list] < 0) continue;
            derive_amvp(sc, pu, list, coding.ref_idx[list], mvp);
            out.pred_flag[list] = 1;
            out.ref_idx[list] = coding.ref_idx[list];
            out.mv[list].x = (int16_t)(
                mvp[coding.mvp_flag[list]].x + coding.mvd[list][0]);
            out.mv[list].y = (int16_t)(
                mvp[coding.mvp_flag[list]].y + coding.mvd[list][1]);
        }
        return out;
    }
}

static int predict_from_list(heic_slice_ctx *sc, const heic_pu *pu,
                             heic_pb_motion motion, int list)
{
    const heic_frame *ref;
    int idx = motion.ref_idx[list];
    if (!motion.pred_flag[list] || idx < 0 || idx >= sc->n_refs[list])
        return -1;
    ref = sc->refs[list][idx];
    if (!ref) return -1;
    if (heic_mc_luma(ref, sc->frame, motion.mv[list],
                     pu->x, pu->y, pu->w, pu->h,
                     sc->mc_scratch, 72u * 72u) != 0)
        return -1;
    if (heic_mc_chroma(ref, sc->frame, motion.mv[list],
                       pu->x, pu->y, pu->w, pu->h,
                       sc->mc_scratch, 72u * 72u) != 0)
        return -1;
    return 0;
}

static int predict_internal_from_list(heic_slice_ctx *sc, const heic_pu *pu,
                                      heic_pb_motion motion, int list)
{
    const heic_frame *ref;
    int16_t *pred = sc->mc_internal + (size_t)list * 3u * 64u * 64u;
    int idx = motion.ref_idx[list];
    if (!motion.pred_flag[list] || idx < 0 || idx >= sc->n_refs[list])
        return -1;
    ref = sc->refs[list][idx];
    if (!ref) return -1;
    if (heic_mc_luma_internal(ref, motion.mv[list],
                              pu->x, pu->y, pu->w, pu->h,
                              pred, 64, sc->mc_scratch, 72u * 72u) != 0)
        return -1;
    if (heic_mc_chroma_internal(ref, motion.mv[list],
                                pu->x, pu->y, pu->w, pu->h,
                                pred + 64u * 64u,
                                pred + 2u * 64u * 64u, 64,
                                sc->mc_scratch, 72u * 72u) != 0)
        return -1;
    return 0;
}

static void blend_internal_block(heic_slice_ctx *sc, const heic_pu *pu)
{
    int16_t *p0 = sc->mc_internal;
    int16_t *p1 = p0 + 3u * 64u * 64u;
    uint32_t x, y;
    int shift_y = 15 - bit_depth_y(sc->sps);
    int max_y = (1 << bit_depth_y(sc->sps)) - 1;
    if (shift_y < 3) shift_y = 3;
    for (y = 0; y < pu->h
                && pu->y + y < (uint32_t)sc->frame->height; y++)
        for (x = 0; x < pu->w
                    && pu->x + x < (uint32_t)sc->frame->width; x++) {
            size_t pos =
                (size_t)(pu->y + y) * sc->frame->y_stride + pu->x + x;
            int v = (p0[y * 64u + x] + p1[y * 64u + x]
                     + (1 << (shift_y - 1))) >> shift_y;
            sc->frame->y[pos] = (uint16_t)clip_int(v, 0, max_y);
        }
    if (sc->frame->chroma_format != 0) {
        int sub_x = sc->frame->chroma_format == 3 ? 1 : 2;
        int sub_y = sc->frame->chroma_format == 1 ? 2 : 1;
        uint32_t cx = pu->x / (uint32_t)sub_x;
        uint32_t cy = pu->y / (uint32_t)sub_y;
        uint32_t cw = pu->w / (uint32_t)sub_x;
        uint32_t ch = pu->h / (uint32_t)sub_y;
        int shift_c = 15 - bit_depth_c(sc->sps);
        int max_c = (1 << bit_depth_c(sc->sps)) - 1;
        uint16_t *planes[2] = {sc->frame->cb, sc->frame->cr};
        int c;
        if (shift_c < 3) shift_c = 3;
        for (c = 0; c < 2; c++) {
            int16_t *a = p0 + (size_t)(c + 1) * 64u * 64u;
            int16_t *b = p1 + (size_t)(c + 1) * 64u * 64u;
            for (y = 0; y < ch
                        && cy + y < (uint32_t)sc->frame->c_height; y++)
                for (x = 0; x < cw
                            && cx + x < (uint32_t)sc->frame->c_width; x++) {
                    size_t pos =
                        (size_t)(cy + y) * sc->frame->c_stride + cx + x;
                    int v = (a[y * 64u + x] + b[y * 64u + x]
                             + (1 << (shift_c - 1))) >> shift_c;
                    planes[c][pos] = (uint16_t)clip_int(v, 0, max_c);
                }
        }
    }
}

static int weighted_uni_value(int sample, int weight, int offset,
                              int denom, int bit_depth, int high_precision)
{
    int shift = 14 - bit_depth;
    int offset_scale = high_precision ? 1 : 1 << (bit_depth - 8);
    int max_val = (1 << bit_depth) - 1;
    int64_t round;
    int64_t v;
    if (shift < 2) shift = 2;
    shift += denom;
    round = (int64_t)1 << (shift - 1);
    v = ((int64_t)sample * weight + round) >> shift;
    v += (int64_t)offset * offset_scale;
    return clip_int((int)v, 0, max_val);
}

static int weighted_bi_value(int a, int b, int w0, int w1,
                             int o0, int o1, int denom, int bit_depth,
                             int high_precision)
{
    int shift = 14 - bit_depth;
    int offset_scale = high_precision ? 1 : 1 << (bit_depth - 8);
    int max_val = (1 << bit_depth) - 1;
    int64_t round;
    int64_t v;
    if (shift < 2) shift = 2;
    shift += denom;
    o0 *= offset_scale;
    o1 *= offset_scale;
    round = (int64_t)(o0 + o1 + 1) * ((int64_t)1 << shift);
    v = ((int64_t)a * w0 + (int64_t)b * w1 + round) >> (shift + 1);
    return clip_int((int)v, 0, max_val);
}

static void apply_internal_weight(heic_slice_ctx *sc, const heic_pu *pu,
                                  heic_pb_motion motion)
{
    const heic_slice_header *sh = sc->sh;
    int high_precision = sc->sps->high_precision_offsets_enabled_flag;
    int bi = motion.pred_flag[0] && motion.pred_flag[1];
    int list = motion.pred_flag[0] ? 0 : 1;
    int ref0 = motion.ref_idx[0], ref1 = motion.ref_idx[1];
    int16_t *p0 = sc->mc_internal;
    int16_t *p1 = p0 + 3u * 64u * 64u;
    int16_t *pred = list ? p1 : p0;
    uint32_t x, y;
    int bd_y = bit_depth_y(sc->sps);
    for (y = 0; y < pu->h
                && pu->y + y < (uint32_t)sc->frame->height; y++)
        for (x = 0; x < pu->w
                    && pu->x + x < (uint32_t)sc->frame->width; x++) {
            size_t pos =
                (size_t)(pu->y + y) * sc->frame->y_stride + pu->x + x;
            int v;
            if (bi)
                v = weighted_bi_value(
                    p0[y * 64u + x], p1[y * 64u + x],
                    sh->luma_weight[0][ref0], sh->luma_weight[1][ref1],
                    sh->luma_offset[0][ref0], sh->luma_offset[1][ref1],
                    sh->luma_log2_weight_denom, bd_y, high_precision);
            else
                v = weighted_uni_value(
                    pred[y * 64u + x], sh->luma_weight[list][motion.ref_idx[list]],
                    sh->luma_offset[list][motion.ref_idx[list]],
                    sh->luma_log2_weight_denom, bd_y, high_precision);
            sc->frame->y[pos] = (uint16_t)v;
        }
    if (sc->frame->chroma_format != 0) {
        int sub_x = sc->frame->chroma_format == 3 ? 1 : 2;
        int sub_y = sc->frame->chroma_format == 1 ? 2 : 1;
        uint32_t cx = pu->x / (uint32_t)sub_x;
        uint32_t cy = pu->y / (uint32_t)sub_y;
        uint32_t cw = pu->w / (uint32_t)sub_x;
        uint32_t ch = pu->h / (uint32_t)sub_y;
        int bd_c = bit_depth_c(sc->sps);
        uint16_t *planes[2] = {sc->frame->cb, sc->frame->cr};
        int c;
        for (c = 0; c < 2; c++) {
            int16_t *a = p0 + (size_t)(c + 1) * 64u * 64u;
            int16_t *b = p1 + (size_t)(c + 1) * 64u * 64u;
            int16_t *src = list ? b : a;
            for (y = 0; y < ch
                        && cy + y < (uint32_t)sc->frame->c_height; y++)
                for (x = 0; x < cw
                            && cx + x < (uint32_t)sc->frame->c_width; x++) {
                    size_t pos =
                        (size_t)(cy + y) * sc->frame->c_stride + cx + x;
                    int v;
                    if (bi)
                        v = weighted_bi_value(
                            a[y * 64u + x], b[y * 64u + x],
                            sh->chroma_weight[0][ref0][c],
                            sh->chroma_weight[1][ref1][c],
                            sh->chroma_offset[0][ref0][c],
                            sh->chroma_offset[1][ref1][c],
                            sh->chroma_log2_weight_denom, bd_c,
                            high_precision);
                    else
                        v = weighted_uni_value(
                            src[y * 64u + x],
                            sh->chroma_weight[list][motion.ref_idx[list]][c],
                            sh->chroma_offset[list][motion.ref_idx[list]][c],
                            sh->chroma_log2_weight_denom, bd_c,
                            high_precision);
                    planes[c][pos] = (uint16_t)v;
                }
        }
    }
}

static int apply_motion(heic_slice_ctx *sc, const heic_pu *pu,
                        heic_pb_motion motion)
{
    int list = motion.pred_flag[0] ? 0 : 1;
    if (!motion.pred_flag[0] && !motion.pred_flag[1]) return -1;
    if (sc->sh->has_pred_weight_table) {
        if (motion.pred_flag[0] &&
            predict_internal_from_list(sc, pu, motion, 0) != 0)
            return -1;
        if (motion.pred_flag[1] &&
            predict_internal_from_list(sc, pu, motion, 1) != 0)
            return -1;
        apply_internal_weight(sc, pu, motion);
        return 0;
    }
    if (motion.pred_flag[0] && motion.pred_flag[1]) {
        if (predict_internal_from_list(sc, pu, motion, 0) != 0 ||
            predict_internal_from_list(sc, pu, motion, 1) != 0)
            return -1;
        blend_internal_block(sc, pu);
        return 0;
    }
    if (predict_from_list(sc, pu, motion, list) != 0) return -1;
    return 0;
}

static int decode_rqt_root_cbf(heic_slice_ctx *sc)
{
    return heic_cabac_decode_bin(&sc->cabac,
                                 &sc->models[HEIC_CTX_RQT_ROOT_CBF])
           != 0;
}

static void mark_inter_pb_boundaries(heic_slice_ctx *sc, int mode,
                                     uint32_t x, uint32_t y, uint32_t n)
{
    uint32_t h = n / 2, q = n / 4;
    if (!sc->deblock_flags) return;
    if (mode == HEIC_PART_NX2N || mode == HEIC_PART_NXN)
        heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x + h, y, n, n, 1);
    if (mode == HEIC_PART_2NXN || mode == HEIC_PART_NXN)
        heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x, y + h, n, n, 0);
    if (mode == HEIC_PART_NLX2N)
        heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x + q, y, n, n, 1);
    if (mode == HEIC_PART_NRX2N)
        heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x + n - q, y, n, n, 1);
    if (mode == HEIC_PART_2NXNU)
        heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x, y + q, n, n, 0);
    if (mode == HEIC_PART_2NXND)
        heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x, y + n - q, n, n, 0);
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

static int decode_cross_component_scale(heic_slice_ctx *sc, uint8_t c_idx)
{
    int chroma = (int)c_idx - 1;
    int log2_abs_plus1 = 0;
    int bin;
    for (bin = 0; bin < 4; bin++) {
        int ctx = HEIC_CTX_LOG2_RES_SCALE_ABS_PLUS1 + chroma * 4 + bin;
        if (!heic_cabac_decode_bin(&sc->cabac, &sc->models[ctx])) break;
        log2_abs_plus1++;
    }
    if (log2_abs_plus1 == 0) return 0;
    return (1 << (log2_abs_plus1 - 1))
           * (1 - 2 * heic_cabac_decode_bin(
                          &sc->cabac,
                          &sc->models[HEIC_CTX_RES_SCALE_SIGN_FLAG + chroma]));
}

static void save_luma_residual16(heic_slice_ctx *sc, const int16_t *residual,
                                 int size)
{
    int i, num;
    uint8_t log2_size = 0;
    if (!sc->luma_residual) return;
    num = size * size;
    for (i = 0; i < num; i++) sc->luma_residual[i] = residual[i];
    while ((1 << log2_size) < size) log2_size++;
    sc->luma_residual_log2 = log2_size;
}

static void save_luma_residual32(heic_slice_ctx *sc, const int32_t *residual,
                                 int size)
{
    uint8_t log2_size = 0;
    if (!sc->luma_residual) return;
    memcpy(sc->luma_residual, residual,
           (size_t)size * size * sizeof(sc->luma_residual[0]));
    while ((1 << log2_size) < size) log2_size++;
    sc->luma_residual_log2 = log2_size;
}

static int32_t cross_component_residual(const heic_slice_ctx *sc,
                                        int x, int y, int res_scale)
{
    int sub_x = sc->frame->chroma_format == 3 ? 1 : 2;
    int sub_y = sc->frame->chroma_format == 1 ? 2 : 1;
    int luma_size = 1 << sc->luma_residual_log2;
    int lx = x * sub_x;
    int ly = y * sub_y;
    int64_t v;
    uint32_t normalized;
    if (!sc->luma_residual || lx >= luma_size || ly >= luma_size) return 0;
    /* The RExt bit-depth normalization uses a fixed-width unsigned intermediate. */
    normalized =
        (uint32_t)sc->luma_residual[ly * luma_size + lx]
        << bit_depth_c(sc->sps);
    normalized >>= bit_depth_y(sc->sps);
    v = (int64_t)res_scale * (int32_t)normalized;
    if (v >= 0) return (int32_t)(v >> 3);
    return (int32_t)-(((-v) + 7) >> 3);
}

static int apply_cross_component_only(heic_slice_ctx *sc, uint32_t x0,
                                      uint32_t y0, uint8_t log2_size,
                                      uint8_t c_idx, int res_scale)
{
    uint16_t *plane = c_idx == 1 ? sc->frame->cb : sc->frame->cr;
    int size = 1 << log2_size;
    int stride = sc->frame->c_stride;
    int max_val = (1 << bit_depth_c(sc->sps)) - 1;
    int py, px;
    if (!plane || res_scale == 0) return 0;
    for (py = 0; py < size && (int)y0 + py < sc->frame->c_height; py++) {
        for (px = 0; px < size && (int)x0 + px < sc->frame->c_width; px++) {
            int32_t v =
                plane[((int)y0 + py) * stride + (int)x0 + px]
                + cross_component_residual(sc, px, py, res_scale);
            if (v < 0) v = 0;
            if (v > max_val) v = max_val;
            plane[((int)y0 + py) * stride + (int)x0 + px] = (uint16_t)v;
        }
    }
    return 0;
}

static int decode_and_apply_residual(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                                     uint8_t log2_size, uint8_t c_idx, int scan_order,
                                     uint8_t intra_mode, int res_scale)
{
    heic_coeff_buf *coeff = sc->coeff;
    int transform_skip = 0;
    int rdpcm_mode = 0;
    int size, num, qp, bd, is_intra_4x4, max_val;
    int extended, max_transform_range;
    uint16_t *plane;
    int stride, plane_w, plane_h;

    if (!coeff) return -1;
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
    extended = sc->sps->extended_precision_processing_flag;
    max_transform_range = extended ? HEIC_MAX(15, bd + 6) : 15;
    if (heic_decode_residual(&sc->cabac, sc->models, log2_size, c_idx, scan_order,
                             sc->pps->sign_data_hiding_enabled_flag,
                             sc->cu_transquant_bypass,
                             sc->pps->transform_skip_enabled_flag,
                             sc->pps->log2_max_transform_skip_block_size,
                             sc->sps->transform_skip_context_enabled_flag,
                             sc->sps->implicit_rdpcm_enabled_flag,
                             sc->sps->explicit_rdpcm_enabled_flag,
                             sc->sps->persistent_rice_adaptation_enabled_flag,
                             sc->sps->cabac_bypass_alignment_enabled_flag,
                             extended, max_transform_range,
                             sc->stat_coeff,
                             sc->cu_pred_mode == HEIC_PRED_INTRA, intra_mode,
                             coeff, &transform_skip, &rdpcm_mode)
        != 0)
        return -1;
    if (coeff->num_nonzero == 0) return 0;

    size = 1 << log2_size;
    num = size * size;
    if (!plane) return 0;
    max_val = (1 << bd) - 1;

    if (sc->sps->transform_skip_rotation_enabled_flag
        && sc->cu_pred_mode == HEIC_PRED_INTRA && log2_size == 2
        && (sc->cu_transquant_bypass || transform_skip)) {
        int i;
        for (i = 0; i < num / 2; i++) {
            if (extended) {
                int32_t t = coeff->coeffs[i];
                coeff->coeffs[i] = coeff->coeffs[num - 1 - i];
                coeff->coeffs[num - 1 - i] = t;
            } else {
                int16_t t = coeff->narrow[i];
                coeff->narrow[i] = coeff->narrow[num - 1 - i];
                coeff->narrow[num - 1 - i] = t;
            }
        }
    }

    if (sc->cu_transquant_bypass) {
        int py, px;
        int32_t *rdpcm = sc->mc_scratch;
        if (rdpcm_mode != 0) {
            if (rdpcm_mode == 1) {
                for (py = 0; py < size; py++) {
                    int32_t sum = 0;
                    for (px = 0; px < size; px++) {
                        int pos = py * size + px;
                        sum += extended ? coeff->coeffs[pos]
                                        : coeff->narrow[pos];
                        rdpcm[py * size + px] = sum;
                    }
                }
            } else {
                for (px = 0; px < size; px++) {
                    int32_t sum = 0;
                    for (py = 0; py < size; py++) {
                        int pos = py * size + px;
                        sum += extended ? coeff->coeffs[pos]
                                        : coeff->narrow[pos];
                        rdpcm[py * size + px] = sum;
                    }
                }
            }
        }
        if (c_idx == 0) {
            if (rdpcm_mode)
                save_luma_residual32(sc, rdpcm, size);
            else if (extended)
                save_luma_residual32(sc, coeff->coeffs, size);
            else
                save_luma_residual16(sc, coeff->narrow, size);
        }
        for (py = 0; py < size; py++) {
            if ((int)y0 + py >= plane_h) break;
            for (px = 0; px < size; px++) {
                int32_t v;
                if ((int)x0 + px >= plane_w) break;
                v = (int32_t)plane[((int)y0 + py) * stride + (int)x0 + px]
                    + (rdpcm_mode ? rdpcm[py * size + px]
                                  : (extended
                                        ? coeff->coeffs[py * size + px]
                                        : coeff->narrow[py * size + px]))
                    + (c_idx ? cross_component_residual(
                                   sc, px, py, res_scale) : 0);
                if (v < 0) v = 0;
                if (v > max_val) v = max_val;
                plane[((int)y0 + py) * stride + (int)x0 + px] = (uint16_t)v;
            }
        }
        return 0;
    }

    if (sc->sps->scaling_list_enabled_flag && !transform_skip) {
        const heic_scaling_list *list =
            sc->pps->pps_scaling_list_data_present_flag
                ? &sc->pps->scaling_list
                : &sc->sps->scaling_list;
        uint8_t matrix_id = (uint8_t)(
            c_idx + (sc->cu_pred_mode == HEIC_PRED_INTRA ? 0 : 3));
        if (extended) {
            heic_dequantize_scaled_extended(
                coeff->coeffs, num, qp, bd, log2_size, max_transform_range,
                list, matrix_id);
        } else {
            heic_dequantize_scaled(coeff->narrow, num, qp, bd, log2_size,
                                   list, matrix_id);
        }
    } else {
        if (extended) {
            heic_dequantize_extended(
                coeff->coeffs, num, qp, bd, log2_size, max_transform_range);
        } else {
            heic_dequantize(coeff->narrow, num, qp, bd, log2_size);
        }
    }

    if (transform_skip) {
        int bd_shift = HEIC_MAX(20 - bd, extended ? 11 : 0);
        int ts_shift = (extended ? HEIC_MIN(5, bd_shift - 2) : 5)
                       + (int)log2_size;
        int rnd, i;
        rnd = bd_shift > 0 ? (1 << (bd_shift - 1)) : 0;
        if (rdpcm_mode == 0) {
            for (i = 0; i < num; i++) {
                int64_t c =
                    (int64_t)(extended ? coeff->coeffs[i] : coeff->narrow[i])
                    * ((int64_t)1 << ts_shift);
                if (extended)
                    sc->mc_scratch[i] = (int32_t)((c + rnd) >> bd_shift);
                else
                    sc->residual_buf[i] = (int16_t)((c + rnd) >> bd_shift);
            }
        } else {
            int py, px;
            int32_t *rdpcm = sc->mc_scratch;
            if (rdpcm_mode == 1) {
                for (py = 0; py < size; py++) {
                    int32_t sum = 0;
                    for (px = 0; px < size; px++) {
                        int pos = py * size + px;
                        int64_t c =
                            (int64_t)(extended
                                ? coeff->coeffs[pos]
                                : coeff->narrow[pos])
                            * ((int64_t)1 << ts_shift);
                        sum += (int32_t)((c + rnd) >> bd_shift);
                        rdpcm[pos] = sum;
                    }
                }
            } else {
                for (px = 0; px < size; px++) {
                    int32_t sum = 0;
                    for (py = 0; py < size; py++) {
                        int pos = py * size + px;
                        int64_t c =
                            (int64_t)(extended
                                ? coeff->coeffs[pos]
                                : coeff->narrow[pos])
                            * ((int64_t)1 << ts_shift);
                        sum += (int32_t)((c + rnd) >> bd_shift);
                        rdpcm[pos] = sum;
                    }
                }
            }
            if (c_idx == 0) save_luma_residual32(sc, rdpcm, size);
            for (py = 0; py < size; py++) {
                if ((int)y0 + py >= plane_h) break;
                for (px = 0; px < size; px++) {
                    int32_t v;
                    if ((int)x0 + px >= plane_w) break;
                    v = (int32_t)plane[((int)y0 + py) * stride + (int)x0 + px]
                        + rdpcm[py * size + px]
                        + (c_idx ? cross_component_residual(
                                       sc, px, py, res_scale) : 0);
                    if (v < 0) v = 0;
                    if (v > max_val) v = max_val;
                    plane[((int)y0 + py) * stride + (int)x0 + px] = (uint16_t)v;
                }
            }
            return 0;
        }
    } else {
        is_intra_4x4 = (log2_size == 2 && c_idx == 0 &&
                        sc->cu_pred_mode == HEIC_PRED_INTRA);
        if (extended) {
            heic_inverse_transform_extended(
                coeff->coeffs, sc->mc_scratch, size, bd,
                max_transform_range, is_intra_4x4);
        } else {
            heic_inverse_transform(
                coeff->narrow, sc->residual_buf, size, bd, is_intra_4x4);
        }
    }

    if (extended) {
        int py, px;
        if (c_idx == 0) save_luma_residual32(sc, sc->mc_scratch, size);
        for (py = 0; py < size; py++) {
            if ((int)y0 + py >= plane_h) break;
            for (px = 0; px < size; px++) {
                int64_t v;
                if ((int)x0 + px >= plane_w) break;
                v = (int64_t)plane[((int)y0 + py) * stride + (int)x0 + px]
                    + sc->mc_scratch[py * size + px]
                    + (c_idx ? cross_component_residual(
                                   sc, px, py, res_scale) : 0);
                if (v < 0) v = 0;
                if (v > max_val) v = max_val;
                plane[((int)y0 + py) * stride + (int)x0 + px] = (uint16_t)v;
            }
        }
        return 0;
    }

    if (c_idx == 0) save_luma_residual16(sc, sc->residual_buf, size);

    /* Clip add if block may extend past plane edge. */
    if (res_scale == 0
        && (int)x0 + size <= plane_w && (int)y0 + size <= plane_h) {
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
                    + sc->residual_buf[py * size + px]
                    + (c_idx ? cross_component_residual(
                                   sc, px, py, res_scale) : 0);
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
                           int intra_split, int inter_split,
                           int cbf_cb_parent, int cbf_cr_parent);

static int decode_tu_leaf(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                          uint8_t log2_size, uint8_t trafo_depth, int cbf_cb,
                          int cbf_cr)
{
    int cbf_luma, sis, scan, chroma_here, is_444;
    uint8_t luma_mode, chroma_mode;
    int ctx_off, ctx_idx;

    if (sc->cu_pred_mode == HEIC_PRED_INTRA || trafo_depth != 0 ||
        cbf_cb || cbf_cr) {
        ctx_off = trafo_depth == 0 ? 1 : 0;
        ctx_idx = HEIC_CTX_CBF_LUMA + ctx_off;
        cbf_luma =
            heic_cabac_decode_bin(&sc->cabac, &sc->models[ctx_idx]) != 0;
    } else {
        cbf_luma = 1;
    }

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

    if (sc->sh->cu_chroma_qp_offset_enabled_flag && (cbf_cb || cbf_cr)
        && !sc->cu_transquant_bypass
        && !sc->is_cu_chroma_qp_offset_coded) {
        int enabled = heic_cabac_decode_bin(
            &sc->cabac,
            &sc->models[HEIC_CTX_CU_CHROMA_QP_OFFSET_FLAG]);
        int idx = 0;
        if (enabled && sc->pps->chroma_qp_offset_list_len > 1) {
            while (idx < 5
                   && heic_cabac_decode_bin(
                       &sc->cabac,
                       &sc->models[HEIC_CTX_CU_CHROMA_QP_OFFSET_IDX]))
                idx++;
        }
        if (idx >= sc->pps->chroma_qp_offset_list_len) return -1;
        sc->is_cu_chroma_qp_offset_coded = 1;
        sc->cu_qp_offset_cb = enabled ? sc->pps->cb_qp_offset_list[idx] : 0;
        sc->cu_qp_offset_cr = enabled ? sc->pps->cr_qp_offset_list[idx] : 0;
        decode_quant_params(sc, x0, sc->cu_base_x, sc->cu_base_y);
    }

    luma_mode = get_intra_mode(sc, x0, y0, 0);
    sis = sc->sps->intra_smoothing_disabled_flag
              ? -1 : sc->sps->strong_intra_smoothing_enabled_flag;
    if (sc->cu_pred_mode == HEIC_PRED_INTRA &&
        predict_intra_block(
            sc, x0, y0, log2_size, luma_mode, 0, sis) != 0)
        return -1;
    scan = sc->cu_pred_mode == HEIC_PRED_INTRA
               ? heic_get_scan_order(log2_size, luma_mode, 0, 0)
               : HEIC_SCAN_DIAG;
    if (cbf_luma) {
        if (decode_and_apply_residual(sc, x0, y0, log2_size, 0, scan,
                                      luma_mode, 0) != 0)
            return -1;
    }

    /* Mark TU edges + QP for deblocking (4x4 grid) */
    {
        uint32_t tu_size = 1u << log2_size;
        heic_mark_tu_boundary(sc->deblock_flags, sc->deblock_stride, sc->deblock_n,
                              x0, y0, tu_size);
        heic_store_deblock_qp(sc->deblock_qp, sc->deblock_stride, sc->deblock_n,
                              x0, y0, tu_size, (int8_t)sc->current_qpy);
        store_cbf(sc, x0, y0, tu_size, cbf_luma || cbf_cb || cbf_cr);
    }

    is_444 = sc->frame->chroma_format == 3;
    chroma_here = is_444 || log2_size >= 3;
    if (chroma_here && chroma_array_type(sc->sps) != 0) {
        uint8_t clog2;
        uint32_t cx, cy;
        int cscan, do_cross_component, res_scale;
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
        cscan = sc->cu_pred_mode == HEIC_PRED_INTRA
                    ? heic_get_scan_order(clog2, chroma_mode, 1, is_444)
                    : HEIC_SCAN_DIAG;
        do_cross_component =
            sc->pps->cross_component_prediction_enabled_flag && cbf_luma
            && (sc->cu_pred_mode != HEIC_PRED_INTRA
                || chroma_mode == luma_mode);
        res_scale = do_cross_component
                        ? decode_cross_component_scale(sc, 1) : 0;
        if (sc->cabac.error) return -1;
        if (sc->cu_pred_mode == HEIC_PRED_INTRA &&
            predict_intra_block(
                sc, cx, cy, clog2, chroma_mode, 1, sis) != 0)
            return -1;
        if (cbf_cb) {
            if (decode_and_apply_residual(sc, cx, cy, clog2, 1, cscan,
                                          chroma_mode, res_scale) != 0)
                return -1;
        } else if (apply_cross_component_only(
                       sc, cx, cy, clog2, 1, res_scale) != 0)
            return -1;
        res_scale = do_cross_component
                        ? decode_cross_component_scale(sc, 2) : 0;
        if (sc->cabac.error) return -1;
        if (sc->cu_pred_mode == HEIC_PRED_INTRA &&
            predict_intra_block(
                sc, cx, cy, clog2, chroma_mode, 2, sis) != 0)
            return -1;
        if (cbf_cr) {
            if (decode_and_apply_residual(sc, cx, cy, clog2, 2, cscan,
                                          chroma_mode, res_scale) != 0)
                return -1;
        } else if (apply_cross_component_only(
                       sc, cx, cy, clog2, 2, res_scale) != 0)
            return -1;
    }
    return 0;
}

static int decode_tt_inner(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                           uint8_t log2_size, uint8_t trafo_depth,
                           int intra_split, int inter_split,
                           int cbf_cb_parent, int cbf_cr_parent)
{
    uint8_t max_depth =
        sc->cu_pred_mode == HEIC_PRED_INTRA
            ? (uint8_t)(sc->sps->max_transform_hierarchy_depth_intra
                        + (intra_split ? 1 : 0))
            : sc->sps->max_transform_hierarchy_depth_inter;
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
    } else if ((log2_size > log2_max || (intra_split && trafo_depth == 0) ||
                (inter_split && trafo_depth == 0))
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
        if (decode_tt_inner(sc, x0, y0, nl, nd, intra_split, inter_split,
                            cbf_cb, cbf_cr) != 0)
            return -1;
        if (decode_tt_inner(sc, x0 + half, y0, nl, nd, intra_split, inter_split,
                            cbf_cb, cbf_cr) != 0)
            return -1;
        if (decode_tt_inner(sc, x0, y0 + half, nl, nd, intra_split, inter_split,
                            cbf_cb, cbf_cr) != 0)
            return -1;
        if (decode_tt_inner(sc, x0 + half, y0 + half, nl, nd, intra_split,
                            inter_split, cbf_cb, cbf_cr)
            != 0)
            return -1;

        /* 4:2:0 chroma at 8x8→4x4 split */
        if (log2_size == 3 && sc->frame->chroma_format != 3 && cat != 0) {
            int sis = sc->sps->intra_smoothing_disabled_flag
                          ? -1 : sc->sps->strong_intra_smoothing_enabled_flag;
            uint8_t cm = get_intra_mode(sc, x0, y0, 1);
            int scan = sc->cu_pred_mode == HEIC_PRED_INTRA
                           ? heic_get_scan_order(2, cm, 1, 0)
                           : HEIC_SCAN_DIAG;
            if (sc->cu_pred_mode == HEIC_PRED_INTRA &&
                predict_intra_block(
                    sc, x0 / 2, y0 / 2, 2, cm, 1, sis) != 0)
                return -1;
            if (cbf_cb
                && decode_and_apply_residual(sc, x0 / 2, y0 / 2, 2, 1, scan,
                                             cm, 0) != 0)
                return -1;
            if (sc->cu_pred_mode == HEIC_PRED_INTRA &&
                predict_intra_block(
                    sc, x0 / 2, y0 / 2, 2, cm, 2, sis) != 0)
                return -1;
            if (cbf_cr
                && decode_and_apply_residual(sc, x0 / 2, y0 / 2, 2, 2, scan,
                                             cm, 0) != 0)
                return -1;
        }
    } else {
        if (decode_tu_leaf(sc, x0, y0, log2_size, trafo_depth, cbf_cb, cbf_cr) != 0)
            return -1;
    }
    return 0;
}

static int pcm_read_bits(const heic_cabac *c, size_t *bit_pos, int n,
                         uint16_t *out)
{
    size_t total_bits;
    uint16_t v = 0;
    int i;
    if (!c || !bit_pos || !out || n < 1 || n > 16 ||
        c->len > SIZE_MAX / 8)
        return -1;
    total_bits = c->len * 8;
    if (*bit_pos > total_bits || (size_t)n > total_bits - *bit_pos)
        return -1;
    for (i = 0; i < n; i++) {
        size_t p = *bit_pos + (size_t)i;
        v = (uint16_t)((v << 1) |
                       ((c->data[p >> 3] >> (7 - (p & 7))) & 1));
    }
    *bit_pos += (size_t)n;
    *out = v;
    return 0;
}

static uint16_t pcm_scale_sample(uint16_t sample, int pcm_depth,
                                 int frame_depth)
{
    if (pcm_depth < frame_depth)
        return (uint16_t)(sample << (frame_depth - pcm_depth));
    if (pcm_depth > frame_depth)
        return (uint16_t)(sample >> (pcm_depth - frame_depth));
    return sample;
}

static int decode_pcm_plane(heic_slice_ctx *sc, uint16_t *plane, int stride,
                            int plane_w, int plane_h, uint32_t x0, uint32_t y0,
                            uint32_t width, uint32_t height, int pcm_depth,
                            int frame_depth, size_t *bit_pos)
{
    uint32_t x, y;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint16_t sample;
            if (pcm_read_bits(&sc->cabac, bit_pos, pcm_depth, &sample) != 0)
                return -1;
            if (plane && x0 + x < (uint32_t)plane_w &&
                y0 + y < (uint32_t)plane_h)
                plane[(size_t)(y0 + y) * (size_t)stride + x0 + x] =
                    pcm_scale_sample(sample, pcm_depth, frame_depth);
        }
    }
    return 0;
}

static void mark_filter_exclusion(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                                  uint32_t size)
{
    uint32_t bx = x0 / 4;
    uint32_t by = y0 / 4;
    uint32_t n4 = size / 4;
    uint32_t dx, dy;
    sc->has_filter_exclusions = 1;
    for (dy = 0; dy < n4; dy++) {
        for (dx = 0; dx < n4; dx++) {
            size_t idx =
                (size_t)(by + dy) * sc->deblock_stride + bx + dx;
            if (idx < sc->deblock_n) sc->pcm_map[idx] = 1;
        }
    }
}

static int decode_pcm_cu(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                         uint8_t log2_cb)
{
    heic_frame *f = sc->frame;
    uint32_t cb_size = 1u << log2_cb;
    int pcm_y_depth = (int)sc->sps->pcm_sample_bit_depth_luma_minus1 + 1;
    int pcm_c_depth = (int)sc->sps->pcm_sample_bit_depth_chroma_minus1 + 1;
    int sub_x = 1, sub_y = 1;
    size_t bit_pos;

    if (pcm_y_depth < 1 || pcm_y_depth > 16 ||
        pcm_c_depth < 1 || pcm_c_depth > 16 ||
        sc->cabac.byte_pos > SIZE_MAX / 8)
        return -1;
    bit_pos = sc->cabac.byte_pos * 8;
    if (decode_pcm_plane(sc, f->y, f->y_stride, f->width, f->height,
                         x0, y0, cb_size, cb_size, pcm_y_depth,
                         bit_depth_y(sc->sps), &bit_pos) != 0)
        goto truncated;

    if (f->chroma_format != 0) {
        if (f->chroma_format == 1 || f->chroma_format == 2) sub_x = 2;
        if (f->chroma_format == 1) sub_y = 2;
        if (decode_pcm_plane(sc, f->cb, f->c_stride, f->c_width, f->c_height,
                             x0 / (uint32_t)sub_x, y0 / (uint32_t)sub_y,
                             cb_size / (uint32_t)sub_x,
                             cb_size / (uint32_t)sub_y, pcm_c_depth,
                             bit_depth_c(sc->sps), &bit_pos) != 0 ||
            decode_pcm_plane(sc, f->cr, f->c_stride, f->c_width, f->c_height,
                             x0 / (uint32_t)sub_x, y0 / (uint32_t)sub_y,
                             cb_size / (uint32_t)sub_x,
                             cb_size / (uint32_t)sub_y, pcm_c_depth,
                             bit_depth_c(sc->sps), &bit_pos) != 0)
            goto truncated;
    }

    if (bit_pos > SIZE_MAX - 7) goto truncated;
    heic_cabac_seek(&sc->cabac, (bit_pos + 7) / 8);
    heic_cabac_reinit(&sc->cabac);

    store_intra_mode(sc, x0, y0, log2_cb, 1, 0);
    store_intra_mode(sc, x0, y0, log2_cb, 1, 1);
    heic_mark_tu_boundary(sc->deblock_flags, sc->deblock_stride,
                          sc->deblock_n, x0, y0, cb_size);
    heic_store_deblock_qp(sc->deblock_qp, sc->deblock_stride,
                          sc->deblock_n, x0, y0, cb_size,
                          (int8_t)sc->current_qpy);
    if (sc->sps->pcm_loop_filter_disabled_flag)
        mark_filter_exclusion(sc, x0, y0, cb_size);
    return 0;

truncated:
    heic_error(sc->hctx, HEIC_SEVERITY_ERROR, "truncated HEVC PCM samples");
    return -1;
}

static int decode_coding_unit(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                              uint8_t log2_cb, uint8_t ct_depth)
{
    uint32_t cb_size = 1u << log2_cb;
    int part_nxn = 0;
    uint8_t luma0 = 1, chroma = 1;
    int intra_split;
    int is_intra_slice = sc->sh->slice_type == HEIC_SLICE_I;
    int cu_skip = 0;

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
    if (sc->cu_transquant_bypass)
        mark_filter_exclusion(sc, x0, y0, cb_size);

    if (!is_intra_slice) cu_skip = decode_cu_skip(sc, x0, y0);
    if (cu_skip) {
        heic_pu pu = {x0, y0, cb_size, cb_size};
        heic_pb_coding coding;
        heic_pb_motion motion;
        sc->cu_pred_mode = HEIC_PRED_SKIP;
        store_pred_mode(sc, x0, y0, cb_size, cb_size, HEIC_PRED_SKIP);
        coding = decode_inter_pu(sc, 1, ct_depth, cb_size, cb_size);
        motion = resolve_motion(sc, coding, &pu, 0, HEIC_PART_2NX2N);
        store_motion(sc, x0, y0, cb_size, cb_size, motion);
        if (apply_motion(sc, &pu, motion) != 0) return -1;
        heic_mark_tu_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x0, y0, cb_size);
        heic_store_deblock_qp(sc->deblock_qp, sc->deblock_stride,
                              sc->deblock_n, x0, y0, cb_size,
                              (int8_t)sc->current_qpy);
        return 0;
    }

    if (!is_intra_slice) {
        sc->cu_pred_mode =
            heic_cabac_decode_bin(&sc->cabac,
                                  &sc->models[HEIC_CTX_PRED_MODE_FLAG])
                ? HEIC_PRED_INTRA
                : HEIC_PRED_INTER;
    } else {
        sc->cu_pred_mode = HEIC_PRED_INTRA;
    }
    store_pred_mode(sc, x0, y0, cb_size, cb_size,
                    (uint8_t)sc->cu_pred_mode);

    if (sc->cu_pred_mode == HEIC_PRED_INTRA &&
        log2_cb == sc->sps->log2_min_cb_size) {
        int pm = decode_part_mode_intra(sc, log2_cb);
        if (pm < 0) return -1;
        part_nxn = pm;
    }

    /* PCM is only allowed for an intra 2Nx2N coding unit. */
    if (sc->cu_pred_mode == HEIC_PRED_INTRA && sc->sps->pcm_enabled_flag) {
        int log2_min =
            (int)sc->sps->log2_min_pcm_luma_coding_block_size_minus3 + 3;
        int log2_max =
            log2_min +
            (int)sc->sps->log2_diff_max_min_pcm_luma_coding_block_size;
        if (!part_nxn && log2_min >= 3 && log2_max <= 6 &&
            log2_cb >= log2_min && log2_cb <= log2_max) {
            if (heic_cabac_decode_terminate(&sc->cabac) != 0) {
                return decode_pcm_cu(sc, x0, y0, log2_cb);
            }
        }
    }

    if (sc->cu_pred_mode == HEIC_PRED_INTRA) {
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
            if (sc->deblock_flags) {
                heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                                      sc->deblock_n, x0 + half, y0, half,
                                      cb_size, 1);
                heic_mark_pb_boundary(sc->deblock_flags, sc->deblock_stride,
                                      sc->deblock_n, x0, y0 + half, cb_size,
                                      half, 0);
            }
        }
        intra_split = part_nxn;
        return decode_tt_inner(sc, x0, y0, log2_cb, 0, intra_split, 0, 1, 1);
    }

    {
        int part_mode = decode_part_mode_inter(sc, log2_cb);
        heic_pu pus[4];
        int n_pu = partition_to_pus(part_mode, x0, y0, cb_size, pus);
        int i, any_merge = 0;
        int has_residual;
        if (n_pu <= 0) return -1;
        for (i = 0; i < n_pu; i++) {
            heic_pb_coding coding = decode_inter_pu(
                sc, 0, ct_depth, pus[i].w, pus[i].h);
            heic_pb_motion motion =
                resolve_motion(sc, coding, &pus[i], i, part_mode);
            any_merge |= coding.merge_flag;
            store_motion(sc, pus[i].x, pus[i].y, pus[i].w, pus[i].h, motion);
            if (apply_motion(sc, &pus[i], motion) != 0) return -1;
        }
        mark_inter_pb_boundaries(sc, part_mode, x0, y0, cb_size);
        has_residual =
            (part_mode == HEIC_PART_2NX2N && any_merge)
                ? 1
                : decode_rqt_root_cbf(sc);
        if (has_residual) {
            int inter_split =
                sc->sps->max_transform_hierarchy_depth_inter == 0 &&
                part_mode != HEIC_PART_2NX2N;
            return decode_tt_inner(sc, x0, y0, log2_cb, 0, 0,
                                   inter_split, 1, 1);
        }
        heic_mark_tu_boundary(sc->deblock_flags, sc->deblock_stride,
                              sc->deblock_n, x0, y0, cb_size);
        heic_store_deblock_qp(sc->deblock_qp, sc->deblock_stride,
                              sc->deblock_n, x0, y0, cb_size,
                              (int8_t)sc->current_qpy);
    }
    return 0;
}

static int decode_cqt(heic_slice_ctx *sc, uint32_t x0, uint32_t y0,
                      uint8_t log2_cb, uint8_t ct_depth)
{
    uint32_t cb;
    uint32_t pw = sc->sps->pic_width_in_luma_samples;
    uint32_t ph = sc->sps->pic_height_in_luma_samples;
    uint8_t log2_min = sc->sps->log2_min_cb_size;
    uint8_t log2_qg;
    uint8_t log2_chroma_qg;
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
    log2_chroma_qg =
        sc->sps->log2_ctb_size > sc->pps->diff_cu_chroma_qp_offset_depth
            ? (uint8_t)(sc->sps->log2_ctb_size
                        - sc->pps->diff_cu_chroma_qp_offset_depth)
            : 0;
    if (sc->sh->cu_chroma_qp_offset_enabled_flag
        && log2_cb >= log2_chroma_qg)
        sc->is_cu_chroma_qp_offset_coded = 0;

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
    uint32_t slice_rs = sc->sh->slice_address;

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
                int scale = 1 << (c_idx == 0
                    ? sc->pps->log2_sao_offset_scale_luma
                    : sc->pps->log2_sao_offset_scale_chroma);
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
                          const heic_frame *const *l0, int n_l0,
                          const heic_frame *const *l1, int n_l1,
                          heic_frame *frame)
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
    if (n_l0 > HEIC_MAX_REF_PICS) n_l0 = HEIC_MAX_REF_PICS;
    if (n_l1 > HEIC_MAX_REF_PICS) n_l1 = HEIC_MAX_REF_PICS;
    sc->n_refs[0] = n_l0;
    sc->n_refs[1] = n_l1;
    if (l0 && n_l0 > 0) {
        int i;
        for (i = 0; i < n_l0; i++) sc->refs[0][i] = l0[i];
    }
    if (l1 && n_l1 > 0) {
        int i;
        for (i = 0; i < n_l1; i++) sc->refs[1][i] = l1[i];
    }
    sc->current_qg_x = -1;
    sc->current_qg_y = -1;
    sc->current_qpy = sh->slice_qp_y;
    sc->last_qpy_in_prev_qg = sh->slice_qp_y;
    sc->qp_y = sh->slice_qp_y;

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
    sc->pred_mode_map = (uint8_t *)heic_zalloc(ctx, pu_n);
    sc->mv_info =
        (heic_pb_motion *)heic_zalloc(ctx, pu_n * sizeof(heic_pb_motion));
    if (!sc->intra_mode_map || !sc->intra_chroma_mode_map ||
        !sc->pred_mode_map || !sc->mv_info)
        return -1;
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
        sc->cbf_map = (uint8_t *)heic_zalloc(ctx, db_n);
        sc->pcm_map = (uint8_t *)heic_zalloc(ctx, db_n);
        sc->deblock_qp = (int8_t *)heic_zalloc(ctx, db_n);
        if (!sc->deblock_flags || !sc->cbf_map || !sc->pcm_map ||
            !sc->deblock_qp)
            return -1;
        {
            size_t i;
            for (i = 0; i < db_n; i++) sc->deblock_qp[i] = (int8_t)sh->slice_qp_y;
        }
    }
    sc->residual_buf =
        (int16_t *)heic_zalloc(ctx, (size_t)HEIC_MAX_COEFF * sizeof(int16_t));
    if (pps->cross_component_prediction_enabled_flag)
        sc->luma_residual =
            (int32_t *)heic_zalloc(ctx, (size_t)HEIC_MAX_COEFF * sizeof(int32_t));
    sc->coeff = (heic_coeff_buf *)heic_zalloc(ctx, sizeof(heic_coeff_buf));
    sc->mc_scratch =
        (int32_t *)heic_zalloc(ctx, 72u * 72u * sizeof(int32_t));
    sc->mc_internal =
        (int16_t *)heic_zalloc(ctx, 6u * 64u * 64u * sizeof(int16_t));
    if (!sc->residual_buf
        || (pps->cross_component_prediction_enabled_flag && !sc->luma_residual)
        || !sc->coeff || !sc->mc_scratch || !sc->mc_internal)
        return -1;
    return 0;
}

static void slice_ctx_free(heic_slice_ctx *sc)
{
    if (!sc || !sc->hctx) return;
    heic_free_buf(sc->hctx, sc->ct_depth_map);
    heic_free_buf(sc->hctx, sc->intra_mode_map);
    heic_free_buf(sc->hctx, sc->intra_chroma_mode_map);
    heic_free_buf(sc->hctx, sc->pred_mode_map);
    heic_free_buf(sc->hctx, sc->mv_info);
    heic_free_buf(sc->hctx, sc->qp_map);
    heic_free_buf(sc->hctx, sc->sao_map);
    heic_free_buf(sc->hctx, sc->deblock_flags);
    heic_free_buf(sc->hctx, sc->cbf_map);
    heic_free_buf(sc->hctx, sc->pcm_map);
    heic_free_buf(sc->hctx, sc->deblock_qp);
    heic_free_buf(sc->hctx, sc->residual_buf);
    heic_free_buf(sc->hctx, sc->luma_residual);
    heic_free_buf(sc->hctx, sc->coeff);
    heic_free_buf(sc->hctx, sc->mc_scratch);
    heic_free_buf(sc->hctx, sc->mc_internal);
    memset(sc, 0, sizeof(*sc));
}

static void slice_ctx_set_refs(
    heic_slice_ctx *sc,
    const heic_frame *const *l0, int n_l0,
    const heic_frame *const *l1, int n_l1)
{
    int i, list;
    for (list = 0; list < 2; list++)
        for (i = 0; i < HEIC_MAX_REF_PICS; i++)
            sc->refs[list][i] = NULL;
    if (n_l0 < 0) n_l0 = 0;
    if (n_l1 < 0) n_l1 = 0;
    if (n_l0 > HEIC_MAX_REF_PICS) n_l0 = HEIC_MAX_REF_PICS;
    if (n_l1 > HEIC_MAX_REF_PICS) n_l1 = HEIC_MAX_REF_PICS;
    sc->n_refs[0] = n_l0;
    sc->n_refs[1] = n_l1;
    for (i = 0; l0 && i < n_l0; i++) sc->refs[0][i] = l0[i];
    for (i = 0; l1 && i < n_l1; i++) sc->refs[1][i] = l1[i];
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
    heic_ctx         *ctx;
    heic_slice_ctx   sc;
    uint32_t         col_bd[65];
    uint32_t         row_bd[65];
    uint32_t         entry_cum[4096];
    heic_ctx_model   wpp_saved[HEIC_NUM_CONTEXTS];
    uint8_t          wpp_stat_coeff[4];
    uint32_t        *tile_scan;
    uint8_t         *ctb_decoded;
    int              tile_scan_n;
    int              n_cols, n_rows;
    int              wpp_have_saved;
    uint32_t         decoded_ctbs;
    int              have_segment;
    int              finished;
    heic_slice_header sh;
    heic_slice_header filter_sh;
} heic_slice_work;

struct heic_hevc_picture {
    heic_slice_work work;
};

heic_hevc_picture *heic_hevc_picture_new(
    heic_ctx *ctx, const heic_sps *sps, const heic_pps *pps,
    const heic_slice_header *sh,
    const heic_frame *const *l0, int n_l0,
    const heic_frame *const *l1, int n_l1, heic_frame *out)
{
    heic_hevc_picture *picture;
    heic_slice_work *work;
    uint32_t total;
    if (!ctx || !sps || !pps || !sh || !out
        || !(total = sps->pic_size_in_ctbs))
        return NULL;
    picture = (heic_hevc_picture *)heic_zalloc(ctx, sizeof(*picture));
    if (!picture) return NULL;
    work = &picture->work;
    work->ctx = ctx;
    work->sh = *sh;
    work->sh.entry_point_offsets = NULL;
    work->sh.num_entry_point_offsets = 0;
    work->filter_sh = work->sh;
    if (slice_ctx_init(&work->sc, ctx, sps, pps, &work->sh,
                       l0, n_l0, l1, n_l1, out) != 0)
        goto fail;
    work->ctb_decoded = (uint8_t *)heic_zalloc(ctx, total);
    if (!work->ctb_decoded) goto fail;
    work->n_cols = 1;
    work->n_rows = 1;
    if (pps->tiles_enabled_flag) {
        if (compute_tile_bd(pps, sps->pic_width_in_ctbs,
                            sps->pic_height_in_ctbs,
                            work->col_bd, work->row_bd,
                            &work->n_cols, &work->n_rows) != 0
            || build_tile_scan(ctx, work->col_bd, work->n_cols,
                               work->row_bd, work->n_rows,
                               &work->tile_scan, &work->tile_scan_n) != 0)
            goto fail;
    }
    return picture;
fail:
    heic_hevc_picture_destroy(picture);
    return NULL;
}

int heic_hevc_picture_decode_segment(
    heic_hevc_picture *picture, const heic_slice_header *sh,
    const uint8_t *data, size_t len,
    const uint32_t *ep_positions, int n_ep,
    const heic_frame *const *l0, int n_l0,
    const heic_frame *const *l1, int n_l1,
    const heic_abort *ab)
{
    heic_slice_work *work;
    heic_slice_ctx *sc;
    heic_ctx *ctx;
    const heic_sps *sps;
    const heic_pps *pps;
    heic_frame *out;
    uint32_t ctb = 0, total, start, pic_w, pic_h, ctb_sz;
    int tiles, wpp;
    int tile_scan_pos = 0;
    int tile_start = 0;
    int n_entry = 0, entry_idx = 0;
    int found_start = 0;
    int i;

    if (!picture || !sh || !data) return -1;
    work = &picture->work;
    sc = &work->sc;
    ctx = sc->hctx;
    sps = sc->sps;
    pps = sc->pps;
    out = sc->frame;
    if (!ctx || !sps || !pps || !out || work->finished) return -1;
    if (sh->slice_type != HEIC_SLICE_I && (!l0 || n_l0 <= 0)) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "predictive slice missing L0 reference frame");
        return -1;
    }
    if (sh->slice_type == HEIC_SLICE_B && (!l1 || n_l1 <= 0)) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "B-slice missing L1 reference frame");
        return -1;
    }
    total = sps->pic_size_in_ctbs;
    start = sh->slice_segment_address;
    if (total == 0 || start >= total) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "slice_segment_address out of range");
        return -1;
    }
    if (work->ctb_decoded[start]) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "overlapping slice segment address");
        return -1;
    }

    work->sh = *sh;
    work->sh.entry_point_offsets = NULL;
    work->sh.num_entry_point_offsets = 0;
    sc->sh = &work->sh;
    slice_ctx_set_refs(sc, l0, n_l0, l1, n_l1);
    if (heic_cabac_new(&sc->cabac, data, len) != 0) return -1;

    pic_w = sps->pic_width_in_ctbs;
    pic_h = sps->pic_height_in_ctbs;
    ctb_sz = ctb_size_px(sps);
    tiles = pps->tiles_enabled_flag;
    wpp = pps->entropy_coding_sync_enabled_flag;

    if (tiles) {
        /* find start in tile scan */
        for (i = 0; i < work->tile_scan_n; i++) {
            if (work->tile_scan[i * 2] == start % pic_w &&
                work->tile_scan[i * 2 + 1] == start / pic_w) {
                tile_scan_pos = i;
                found_start = 1;
                break;
            }
        }
        if (!found_start) return -1;
        sc->ctb_x = work->tile_scan[tile_scan_pos * 2];
        sc->ctb_y = work->tile_scan[tile_scan_pos * 2 + 1];
        if (tile_scan_pos == 0) {
            tile_start = 1;
        } else {
            uint32_t prev_x = work->tile_scan[(tile_scan_pos - 1) * 2];
            uint32_t prev_y = work->tile_scan[(tile_scan_pos - 1) * 2 + 1];
            tile_start =
                get_tile_id(work->col_bd, work->n_cols,
                            work->row_bd, work->n_rows,
                            sc->ctb_x, sc->ctb_y)
                != get_tile_id(work->col_bd, work->n_cols,
                               work->row_bd, work->n_rows,
                               prev_x, prev_y);
        }
    } else {
        sc->ctb_y = start / pic_w;
        sc->ctb_x = start % pic_w;
    }

    if (!sh->dependent_slice_segment_flag || tile_start) {
        heic_cabac_init_contexts(sc->models, sh->slice_type,
                                 sh->cabac_init_flag, sh->slice_qp_y);
    } else if (!work->have_segment) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "dependent slice precedes independent slice");
        return -1;
    }
    if (!sh->dependent_slice_segment_flag) {
        sc->current_qpy = sh->slice_qp_y;
        sc->last_qpy_in_prev_qg = sh->slice_qp_y;
        sc->current_qg_x = -1;
        sc->current_qg_y = -1;
        sc->is_cu_qp_delta_coded = 0;
        sc->cu_qp_delta = 0;
        sc->is_cu_chroma_qp_offset_coded = 0;
        sc->cu_qp_offset_cb = 0;
        sc->cu_qp_offset_cr = 0;
        memset(sc->stat_coeff, 0, sizeof(sc->stat_coeff));
        work->filter_sh = work->sh;
    } else if (wpp && sc->ctb_x == 0 && sc->ctb_y > 0
               && work->wpp_have_saved) {
        memcpy(sc->models, work->wpp_saved, sizeof(sc->models));
        memcpy(sc->stat_coeff, work->wpp_stat_coeff, sizeof(sc->stat_coeff));
    }
    work->have_segment = 1;

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
        uint32_t addr_rs = sc->ctb_y * pic_w + sc->ctb_x;

        if (heic_abort_check(ab)) return -1;
        if (heic_cabac_overread(&sc->cabac)) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "CABAC overread during CTU decode");
            return -1;
        }
        if (addr_rs >= total || work->ctb_decoded[addr_rs]) {
            heic_error(ctx, HEIC_SEVERITY_ERROR,
                       "slice segment overlaps decoded CTU");
            return -1;
        }
        if (decode_ctu(sc, x, y) != 0 || sc->cabac.error) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "CTU decode failed at (%u,%u) ctu=%u",
                       sc->ctb_x, sc->ctb_y, ctb);
            return -1;
        }
        work->ctb_decoded[addr_rs] = 1;
        ctb++;

        /* WPP: save contexts after CTB column 1 (for next row restore) */
        if (wpp && sc->ctb_x == 1 && sc->ctb_y + 1 < pic_h) {
            memcpy(work->wpp_saved, sc->models, sizeof(work->wpp_saved));
            memcpy(work->wpp_stat_coeff, sc->stat_coeff,
                   sizeof(work->wpp_stat_coeff));
            work->wpp_have_saved = 1;
        }

        end_flag = heic_cabac_decode_terminate(&sc->cabac);
        if (end_flag) break;

        /* advance CTB */
        if (tiles) {
            tile_scan_pos++;
            if (tile_scan_pos >= work->tile_scan_n) break;
            sc->ctb_x = work->tile_scan[tile_scan_pos * 2];
            sc->ctb_y = work->tile_scan[tile_scan_pos * 2 + 1];
            {
                uint32_t pt =
                    get_tile_id(work->col_bd, work->n_cols,
                                work->row_bd, work->n_rows,
                                prev_x, prev_y);
                uint32_t ct =
                    get_tile_id(work->col_bd, work->n_cols,
                                work->row_bd, work->n_rows,
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
                    sc->is_cu_chroma_qp_offset_coded = 0;
                    sc->cu_qp_offset_cb = 0;
                    sc->cu_qp_offset_cr = 0;
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
                    if (work->wpp_have_saved)
                        memcpy(sc->models, work->wpp_saved, sizeof(sc->models));
                    if (work->wpp_have_saved)
                        memcpy(sc->stat_coeff, work->wpp_stat_coeff,
                               sizeof(sc->stat_coeff));
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

    heic_error(ctx, HEIC_SEVERITY_INFO, "%c-slice decoded %u CTUs",
               sh->slice_type == HEIC_SLICE_I ? 'I' : 'P', (unsigned)ctb);
    work->decoded_ctbs += ctb;
    return 0;
}

int heic_hevc_picture_finish(heic_hevc_picture *picture)
{
    heic_slice_work *work;
    heic_slice_ctx *sc;
    const heic_sps *sps;
    const heic_pps *pps;
    const heic_slice_header *sh;
    heic_frame *out;
    heic_ctx *ctx;
    uint32_t ctb_sz;
    int i;
    if (!picture) return -1;
    work = &picture->work;
    sc = &work->sc;
    ctx = sc->hctx;
    sps = sc->sps;
    pps = sc->pps;
    sh = &work->filter_sh;
    out = sc->frame;
    if (!ctx || !sps || !pps || !out || work->finished
        || !work->have_segment
        || work->decoded_ctbs != sps->pic_size_in_ctbs) {
        if (ctx && sps && work->decoded_ctbs != sps->pic_size_in_ctbs)
            heic_error(ctx, HEIC_SEVERITY_ERROR,
                       "incomplete HEVC picture: decoded %u of %u CTUs",
                       (unsigned)work->decoded_ctbs,
                       (unsigned)sps->pic_size_in_ctbs);
        return -1;
    }
    if (sh->slice_type != HEIC_SLICE_I) {
        int list;
        for (list = 0; list < 2; list++) {
            for (i = 0; i < HEIC_MAX_REF_PICS; i++)
                out->ref_poc[list][i] = INT_MIN;
            for (i = 0; i < sc->n_refs[list] && i < HEIC_MAX_REF_PICS; i++)
                if (sc->refs[list][i] && sc->refs[list][i]->poc_valid) {
                    out->ref_poc[list][i] = sc->refs[list][i]->poc;
                    out->ref_long_term[list][i] =
                        (uint8_t)active_ref_is_long_term(sc, list, i);
                }
        }
    }

    ctb_sz = ctb_size_px(sps);
    if (!sh->slice_deblocking_filter_disabled_flag && sc->deblock_flags
        && sc->deblock_qp) {
        int beta = (int)sh->slice_beta_offset_div2 * 2;
        int tc = (int)sh->slice_tc_offset_div2 * 2;
        int cb_off =
            (int)pps->pps_cb_qp_offset + (int)sh->slice_cb_qp_offset;
        int cr_off =
            (int)pps->pps_cr_qp_offset + (int)sh->slice_cr_qp_offset;
        heic_apply_deblock(
            out, sc->deblock_flags, sc->deblock_qp, sc->deblock_stride,
            beta, tc, cb_off, cr_off,
            sh->slice_type == HEIC_SLICE_I ? NULL : sc->pred_mode_map,
            sh->slice_type == HEIC_SLICE_I ? NULL : sc->mv_info,
            sc->intra_mode_stride, min_pu_size(sps),
            sh->slice_type == HEIC_SLICE_I ? NULL : sc->cbf_map,
            sh->slice_type == HEIC_SLICE_I ? NULL : out->ref_poc,
            sc->has_filter_exclusions ? sc->pcm_map : NULL);
    }
    if (sps->sample_adaptive_offset_enabled_flag && sc->sao_map) {
        heic_apply_sao(
            ctx, out, sc->sao_map, sps->pic_width_in_ctbs,
            sps->pic_height_in_ctbs, ctb_sz,
            sc->has_filter_exclusions ? sc->pcm_map : NULL,
            sc->deblock_stride);
    }

    if (sh->slice_type != HEIC_SLICE_I) {
        out->motion = sc->mv_info;
        out->motion_pred_mode = sc->pred_mode_map;
        out->motion_n = sc->intra_mode_n;
        out->motion_stride = sc->intra_mode_stride;
        out->motion_min_pu = min_pu_size(sps);
        sc->mv_info = NULL;
        sc->pred_mode_map = NULL;
    }
    work->finished = 1;
    return 0;
}

void heic_hevc_picture_destroy(heic_hevc_picture *picture)
{
    heic_slice_work *work;
    heic_ctx *ctx;
    if (!picture) return;
    work = &picture->work;
    ctx = work->ctx;
    if (!ctx) return;
    heic_free_buf(ctx, work->tile_scan);
    heic_free_buf(ctx, work->ctb_decoded);
    slice_ctx_free(&work->sc);
    heic_free_buf(ctx, picture);
}
