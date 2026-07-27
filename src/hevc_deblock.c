/* hevc_deblock.c -- Deblocking filter (H.265 8.7.2)
 *
 * Port of imazen/heic deblock.rs. Handles intra and single-reference P pictures.
 */
#include "heic_internal.h"

#define HEIC_DEBLOCK_FLAG_VERT      1
#define HEIC_DEBLOCK_FLAG_HORIZ     2
#define HEIC_DEBLOCK_PB_EDGE_VERT   4
#define HEIC_DEBLOCK_PB_EDGE_HORIZ  8

static const uint16_t BETA_PRIME[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24,
    26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56,
    58, 60, 62, 64,
};

static const uint16_t TC_PRIME[54] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  3,
     3,  3,  3,  4,  4,  4,  5,  5,  6,  6,  7,  8,  9, 10, 11, 13,
    14, 16, 18, 20, 22, 24,
};

static const int CHROMA_QP_TABLE[13] = {
    29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37,
};

static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int iabs(int v) { return v < 0 ? -v : v; }

static int chroma_qp_mapping(int qp_i)
{
    if (qp_i < 30) return qp_i;
    if (qp_i >= 43) return qp_i - 6;
    return CHROMA_QP_TABLE[qp_i - 30];
}

static int pcm_at(const uint8_t *pcm_map, uint32_t stride,
                  uint32_t x, uint32_t y)
{
    if (!pcm_map || stride == 0) return 0;
    return pcm_map[(size_t)(y / 4) * stride + x / 4] != 0;
}

static int deblock_edge_allowed(
    const heic_ctb_filter_info *filter_map, uint32_t width_ctbs,
    uint32_t ctb_size, int loop_filter_across_tiles,
    uint32_t x, uint32_t y, int vertical,
    const heic_ctb_filter_info **q_info)
{
    uint32_t qx, qy, px, py;
    const heic_ctb_filter_info *p, *q;
    if (q_info) *q_info = NULL;
    if (!filter_map || !width_ctbs || !ctb_size) return 1;
    qx = x / ctb_size;
    qy = y / ctb_size;
    px = vertical ? (x - 1) / ctb_size : qx;
    py = vertical ? qy : (y - 1) / ctb_size;
    q = &filter_map[(size_t)qy * width_ctbs + qx];
    if (q_info) *q_info = q;
    if (q->deblocking_disabled) return 0;
    if (px == qx && py == qy) return 1;
    p = &filter_map[(size_t)py * width_ctbs + px];
    if (p->slice_address != q->slice_address &&
        !q->loop_filter_across_slices)
        return 0;
    if (p->tile_id != q->tile_id && !loop_filter_across_tiles)
        return 0;
    return 1;
}

static int mv_diff_ge4(heic_mv a, heic_mv b)
{
    return iabs((int)a.x - (int)b.x) >= 4 ||
           iabs((int)a.y - (int)b.y) >= 4;
}

static int resolve_ref_poc(const int ref_poc[2][HEIC_MAX_REF_PICS],
                           heic_pb_motion motion, int list)
{
    int ref = motion.ref_idx[list];
    if (!motion.pred_flag[list] || !ref_poc ||
        ref < 0 || ref >= HEIC_MAX_REF_PICS)
        return INT_MIN;
    return ref_poc[list][ref];
}

static int compute_bs(uint32_t x, uint32_t y, int vertical,
                      int is_transform_edge, const uint8_t *pred_mode,
                      const heic_pb_motion *mv_info, uint32_t pu_stride,
                      uint32_t min_pu, const uint8_t *cbf_map,
                      uint32_t cbf_stride,
                      const int ref_poc[2][HEIC_MAX_REF_PICS])
{
    uint32_t px = vertical ? x - 1 : x;
    uint32_t py = vertical ? y : y - 1;
    uint32_t qx = x, qy = y;
    size_t pi, qi;
    heic_pb_motion mp, mq;
    int count_p, count_q;
    int rp0, rp1, rq0, rq1;
    heic_mv mp0 = {0, 0}, mp1 = {0, 0};
    heic_mv mq0 = {0, 0}, mq1 = {0, 0};
    if (!pred_mode || !mv_info || !min_pu) return 2;
    pi = (size_t)(py / min_pu) * pu_stride + px / min_pu;
    qi = (size_t)(qy / min_pu) * pu_stride + qx / min_pu;
    if (pred_mode[pi] == 1 || pred_mode[qi] == 1) return 2;
    if (is_transform_edge && cbf_map) {
        size_t cp = (size_t)(py / 4) * cbf_stride + px / 4;
        size_t cq = (size_t)(qy / 4) * cbf_stride + qx / 4;
        if (cbf_map[cp] || cbf_map[cq]) return 1;
    }
    mp = mv_info[pi];
    mq = mv_info[qi];
    count_p = mp.pred_flag[0] + mp.pred_flag[1];
    count_q = mq.pred_flag[0] + mq.pred_flag[1];
    if (count_p != count_q) return 1;

    rp0 = resolve_ref_poc(ref_poc, mp, 0);
    rp1 = resolve_ref_poc(ref_poc, mp, 1);
    rq0 = resolve_ref_poc(ref_poc, mq, 0);
    rq1 = resolve_ref_poc(ref_poc, mq, 1);
    if (!((rp0 == rq0 && rp1 == rq1) ||
          (rp0 == rq1 && rp1 == rq0)))
        return 1;

    if (mp.pred_flag[0]) mp0 = mp.mv[0];
    if (mp.pred_flag[1]) mp1 = mp.mv[1];
    if (mq.pred_flag[0]) mq0 = mq.mv[0];
    if (mq.pred_flag[1]) mq1 = mq.mv[1];
    if (rp0 != rp1) {
        if (rp0 == rq0) {
            if (mv_diff_ge4(mp0, mq0) || mv_diff_ge4(mp1, mq1))
                return 1;
        } else {
            if (mv_diff_ge4(mp0, mq1) || mv_diff_ge4(mp1, mq0))
                return 1;
        }
    } else {
        int same_order_diff =
            mv_diff_ge4(mp0, mq0) || mv_diff_ge4(mp1, mq1);
        int cross_order_diff =
            mv_diff_ge4(mp0, mq1) || mv_diff_ge4(mp1, mq0);
        if (same_order_diff && cross_order_diff) return 1;
    }
    return 0;
}

static void filter_edge_luma(heic_frame *frame, uint32_t x, uint32_t y,
                             int vertical, int qp_p, int qp_q, int beta_offset,
                             int tc_offset, int bs, int filter_p, int filter_q)
{
    int bit_depth = frame->bit_depth;
    int max_val = (1 << bit_depth) - 1;
    int qp_l, q_beta, beta, q_tc, tc;
    int stride = frame->y_stride;
    uint16_t *plane = frame->y;
    size_t step_along, step_across, base_q, base_p, last_q;
    int p0_0, p1_0, p2_0, p3_0, q0_0, q1_0, q2_0, q3_0;
    int p0_3, p1_3, p2_3, p3_3, q0_3, q1_3, q2_3, q3_3;
    int dp0, dp3, dq0, dq3, dpq0, dpq3, dp, dq, d;
    int d_sam0, d_sam3, strong, d_ep, d_eq;
    size_t k3;
    int k;

    if (!plane || stride <= 0 || (!filter_p && !filter_q)) return;

    qp_l = (qp_q + qp_p + 1) >> 1;
    q_beta = clampi(qp_l + beta_offset, 0, 51);
    beta = ((int)BETA_PRIME[q_beta]) << (bit_depth - 8);
    q_tc = clampi(qp_l + 2 * (bs - 1) + tc_offset, 0, 53);
    tc = ((int)TC_PRIME[q_tc]) << (bit_depth - 8);
    if (tc == 0) return;

    if (vertical) {
        step_along = (size_t)stride;
        step_across = 1;
        base_q = (size_t)y * (size_t)stride + (size_t)x;
    } else {
        step_along = 1;
        step_across = (size_t)stride;
        base_q = (size_t)y * (size_t)stride + (size_t)x;
    }
    if (base_q < step_across) return;
    base_p = base_q - step_across;
    if (base_p < 3 * step_across) return;
    last_q = base_q + 3 * step_along + 3 * step_across;
    if (last_q >= (size_t)stride * (size_t)frame->height) return;

    k3 = 3 * step_along;
    p0_0 = plane[base_p];
    p1_0 = plane[base_p - step_across];
    p2_0 = plane[base_p - 2 * step_across];
    p3_0 = plane[base_p - 3 * step_across];
    q0_0 = plane[base_q];
    q1_0 = plane[base_q + step_across];
    q2_0 = plane[base_q + 2 * step_across];
    q3_0 = plane[base_q + 3 * step_across];
    p0_3 = plane[base_p + k3];
    p1_3 = plane[base_p + k3 - step_across];
    p2_3 = plane[base_p + k3 - 2 * step_across];
    p3_3 = plane[base_p + k3 - 3 * step_across];
    q0_3 = plane[base_q + k3];
    q1_3 = plane[base_q + k3 + step_across];
    q2_3 = plane[base_q + k3 + 2 * step_across];
    q3_3 = plane[base_q + k3 + 3 * step_across];

    dp0 = iabs(p2_0 - 2 * p1_0 + p0_0);
    dp3 = iabs(p2_3 - 2 * p1_3 + p0_3);
    dq0 = iabs(q2_0 - 2 * q1_0 + q0_0);
    dq3 = iabs(q2_3 - 2 * q1_3 + q0_3);
    dpq0 = dp0 + dq0;
    dpq3 = dp3 + dq3;
    dp = dp0 + dp3;
    dq = dq0 + dq3;
    d = dpq0 + dpq3;
    if (d >= beta) return;

    d_sam0 = (2 * dpq0 < (beta >> 2)) &&
             (iabs(p3_0 - p0_0) + iabs(q0_0 - q3_0) < (beta >> 3)) &&
             (iabs(p0_0 - q0_0) < ((5 * tc + 1) >> 1));
    d_sam3 = (2 * dpq3 < (beta >> 2)) &&
             (iabs(p3_3 - p0_3) + iabs(q0_3 - q3_3) < (beta >> 3)) &&
             (iabs(p0_3 - q0_3) < ((5 * tc + 1) >> 1));
    strong = d_sam0 && d_sam3;
    d_ep = dp < ((beta + (beta >> 1)) >> 3);
    d_eq = dq < ((beta + (beta >> 1)) >> 3);

    if (filter_p && filter_q &&
        heic_simd_luma_filter4(plane, base_p, base_q, step_along, step_across,
                               strong, d_ep, d_eq, tc, max_val))
        return;

    for (k = 0; k < 4; k++) {
        size_t k_off = (size_t)k * step_along;
        int p0 = plane[base_p + k_off];
        int p1 = plane[base_p + k_off - step_across];
        int p2 = plane[base_p + k_off - 2 * step_across];
        int q0 = plane[base_q + k_off];
        int q1 = plane[base_q + k_off + step_across];
        int q2 = plane[base_q + k_off + 2 * step_across];

        if (strong) {
            int p3 = plane[base_p + k_off - 3 * step_across];
            int q3 = plane[base_q + k_off + 3 * step_across];
            int tc2 = 2 * tc;
            int p0_f = clampi(clampi((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3,
                                     p0 - tc2, p0 + tc2),
                              0, max_val);
            int p1_f = clampi(clampi((p2 + p1 + p0 + q0 + 2) >> 2, p1 - tc2, p1 + tc2),
                              0, max_val);
            int p2_f = clampi(clampi((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3,
                                     p2 - tc2, p2 + tc2),
                              0, max_val);
            int q0_f = clampi(clampi((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3,
                                     q0 - tc2, q0 + tc2),
                              0, max_val);
            int q1_f = clampi(clampi((p0 + q0 + q1 + q2 + 2) >> 2, q1 - tc2, q1 + tc2),
                              0, max_val);
            int q2_f = clampi(clampi((p0 + q0 + q1 + 3 * q2 + 2 * q3 + 4) >> 3,
                                     q2 - tc2, q2 + tc2),
                              0, max_val);
            if (filter_p) {
                plane[base_p + k_off] = (uint16_t)p0_f;
                plane[base_p + k_off - step_across] = (uint16_t)p1_f;
                plane[base_p + k_off - 2 * step_across] = (uint16_t)p2_f;
            }
            if (filter_q) {
                plane[base_q + k_off] = (uint16_t)q0_f;
                plane[base_q + k_off + step_across] = (uint16_t)q1_f;
                plane[base_q + k_off + 2 * step_across] = (uint16_t)q2_f;
            }
        } else {
            int delta = (9 * (q0 - p0) - 3 * (q1 - p1) + 8) >> 4;
            if (iabs(delta) < 10 * tc) {
                delta = clampi(delta, -tc, tc);
                if (filter_p)
                    plane[base_p + k_off] =
                        (uint16_t)clampi(p0 + delta, 0, max_val);
                if (filter_q)
                    plane[base_q + k_off] =
                        (uint16_t)clampi(q0 - delta, 0, max_val);
                if (filter_p && d_ep) {
                    int delta_p = clampi(((((p2 + p0 + 1) >> 1) - p1 + delta) >> 1),
                                         -(tc >> 1), tc >> 1);
                    plane[base_p + k_off - step_across] =
                        (uint16_t)clampi(p1 + delta_p, 0, max_val);
                }
                if (filter_q && d_eq) {
                    int delta_q = clampi(((((q2 + q0 + 1) >> 1) - q1 - delta) >> 1),
                                         -(tc >> 1), tc >> 1);
                    plane[base_q + k_off + step_across] =
                        (uint16_t)clampi(q1 + delta_q, 0, max_val);
                }
            }
        }
    }
}

static void apply_chroma_deblocking(heic_frame *frame, const uint8_t *flags,
                                    const int8_t *qp_map, uint32_t deblock_stride,
                                    int cb_qp_offset, int cr_qp_offset,
                                    const heic_ctb_filter_info *filter_map,
                                    uint32_t width_ctbs, uint32_t ctb_size,
                                    int loop_filter_across_tiles,
                                    const uint8_t *pred_mode,
                                    const heic_pb_motion *mv_info,
                                    uint32_t pu_stride, uint32_t min_pu,
                                    const uint8_t *cbf_map,
                                    const int ref_poc[2][HEIC_MAX_REF_PICS],
                                    const uint8_t *pcm_map)
{
    int w = frame->width, h = frame->height;
    int bit_depth_c = frame->chroma_bit_depth;
    int max_val = (1 << bit_depth_c) - 1;
    int sub_x, sub_y;
    int c_stride, c_height, c_width;
    uint32_t x_step_vert, y_step_vert, x_step_horiz, y_step_horiz;
    uint32_t x, y;
    int vert_edge_mask = HEIC_DEBLOCK_FLAG_VERT | HEIC_DEBLOCK_PB_EDGE_VERT;
    int horiz_edge_mask = HEIC_DEBLOCK_FLAG_HORIZ | HEIC_DEBLOCK_PB_EDGE_HORIZ;

    if (frame->chroma_format == 0 || !frame->cb || !frame->cr) return;
    switch (frame->chroma_format) {
    case 1: sub_x = 2; sub_y = 2; break;
    case 2: sub_x = 2; sub_y = 1; break;
    case 3: sub_x = 1; sub_y = 1; break;
    default: return;
    }
    c_stride = frame->c_stride;
    c_height = frame->c_height;
    c_width = frame->c_width;
    if (c_stride <= 0 || c_height <= 0 || c_width <= 0) return;

    x_step_vert = (uint32_t)(8 * sub_x);
    y_step_vert = (uint32_t)(4 * sub_y);
    x_step_horiz = (uint32_t)(4 * sub_x);
    y_step_horiz = (uint32_t)(8 * sub_y);

    for (x = x_step_vert; x < (uint32_t)w; x += x_step_vert) {
        for (y = 0; y < (uint32_t)h; y += y_step_vert) {
            uint32_t bx = x / 4, by = y / 4;
            size_t idx = (size_t)by * deblock_stride + bx;
            int bs, qp_q, qp_p, c_idx;
            int filter_p, filter_q;
            uint32_t cx, cy;
            const heic_ctb_filter_info *q_filter;

            if (!deblock_edge_allowed(
                    filter_map, width_ctbs, ctb_size,
                    loop_filter_across_tiles, x, y, 1, &q_filter))
                continue;
            if ((flags[idx] & (uint8_t)vert_edge_mask) == 0) continue;
            bs = compute_bs(x, y, 1,
                            (flags[idx] & HEIC_DEBLOCK_FLAG_VERT) != 0,
                            pred_mode, mv_info, pu_stride, min_pu, cbf_map,
                            deblock_stride, ref_poc);
            if (bs < 2) continue;
            filter_p = !pcm_at(pcm_map, deblock_stride, x - 1, y);
            filter_q = !pcm_at(pcm_map, deblock_stride, x, y);
            if (!filter_p && !filter_q) continue;
            qp_q = (int)qp_map[idx];
            qp_p = bx > 0 ? (int)qp_map[(size_t)by * deblock_stride + (bx - 1)] : qp_q;
            cx = x / (uint32_t)sub_x;
            cy = y / (uint32_t)sub_y;

            if (cx < 2 || (int)cx + 1 >= c_stride) continue;
            for (c_idx = 0; c_idx < 2; c_idx++) {
                int qp_offset = c_idx == 0 ? cb_qp_offset : cr_qp_offset;
                int qp_i = ((qp_q + qp_p + 1) >> 1) + qp_offset;
                int qp_c = chroma_qp_mapping(qp_i);
                int tc_offset = q_filter ? q_filter->tc_offset : 0;
                int q_tc = clampi(qp_c + 2 + tc_offset, 0, 53);
                int tc = ((int)TC_PRIME[q_tc]) << (bit_depth_c - 8);
                uint16_t *plane = c_idx == 0 ? frame->cb : frame->cr;
                uint32_t k, num;
                size_t ci = (size_t)cx;

                if (tc == 0) continue;
                num = 4;
                if (cy + num > (uint32_t)c_height) num = (uint32_t)c_height - cy;
                /* Vertical edge: samples along y (stride), across x (1). */
                if (filter_p && filter_q && num == 4 &&
                    heic_simd_chroma_edge4(plane, c_stride,
                                           (size_t)cy * (size_t)c_stride + ci, 1, tc,
                                           max_val, 1))
                    continue;
                for (k = 0; k < num; k++) {
                    size_t base = ((size_t)cy + k) * (size_t)c_stride + ci;
                    int p1 = plane[base - 2];
                    int p0 = plane[base - 1];
                    int q0 = plane[base];
                    int q1 = plane[base + 1];
                    int delta = ((q0 - p0) * 4 + p1 - q1 + 4) >> 3;
                    int p0n, q0n;
                    if (delta > tc) delta = tc;
                    else if (delta < -tc) delta = -tc;
                    p0n = p0 + delta;
                    q0n = q0 - delta;
                    if (p0n < 0) p0n = 0;
                    else if (p0n > max_val) p0n = max_val;
                    if (q0n < 0) q0n = 0;
                    else if (q0n > max_val) q0n = max_val;
                    if (filter_p) plane[base - 1] = (uint16_t)p0n;
                    if (filter_q) plane[base] = (uint16_t)q0n;
                }
            }
        }
    }

    for (y = y_step_horiz; y < (uint32_t)h; y += y_step_horiz) {
        for (x = 0; x < (uint32_t)w; x += x_step_horiz) {
            uint32_t bx = x / 4, by = y / 4;
            size_t idx = (size_t)by * deblock_stride + bx;
            int bs, qp_q, qp_p, c_idx;
            int filter_p, filter_q;
            uint32_t cx, cy;
            const heic_ctb_filter_info *q_filter;

            if (!deblock_edge_allowed(
                    filter_map, width_ctbs, ctb_size,
                    loop_filter_across_tiles, x, y, 0, &q_filter))
                continue;
            if ((flags[idx] & (uint8_t)horiz_edge_mask) == 0) continue;
            bs = compute_bs(x, y, 0,
                            (flags[idx] & HEIC_DEBLOCK_FLAG_HORIZ) != 0,
                            pred_mode, mv_info, pu_stride, min_pu, cbf_map,
                            deblock_stride, ref_poc);
            if (bs < 2) continue;
            filter_p = !pcm_at(pcm_map, deblock_stride, x, y - 1);
            filter_q = !pcm_at(pcm_map, deblock_stride, x, y);
            if (!filter_p && !filter_q) continue;
            qp_q = (int)qp_map[idx];
            qp_p = by > 0 ? (int)qp_map[(size_t)(by - 1) * deblock_stride + bx] : qp_q;
            cx = x / (uint32_t)sub_x;
            cy = y / (uint32_t)sub_y;
            if (cy < 2 || (int)cy + 1 >= c_height) continue;

            for (c_idx = 0; c_idx < 2; c_idx++) {
                int qp_offset = c_idx == 0 ? cb_qp_offset : cr_qp_offset;
                int qp_i = ((qp_q + qp_p + 1) >> 1) + qp_offset;
                int qp_c = chroma_qp_mapping(qp_i);
                int tc_offset = q_filter ? q_filter->tc_offset : 0;
                int q_tc = clampi(qp_c + 2 + tc_offset, 0, 53);
                int tc = ((int)TC_PRIME[q_tc]) << (bit_depth_c - 8);
                uint16_t *plane = c_idx == 0 ? frame->cb : frame->cr;
                uint32_t k, num;
                size_t row_q = (size_t)cy;
                size_t row_p = row_q - 1;
                size_t cs = (size_t)c_stride;

                if (tc == 0) continue;
                num = 4;
                if (cx + num > (uint32_t)c_width) num = (uint32_t)c_width - cx;
                /* Horizontal edge: samples along x (1), across y (stride). */
                if (filter_p && filter_q && num == 4 &&
                    heic_simd_chroma_edge4(plane, c_stride, row_q * cs + (size_t)cx,
                                           (int)cs, tc, max_val, 0))
                    continue;
                for (k = 0; k < num; k++) {
                    size_t col = (size_t)(cx + k);
                    int p1 = plane[(row_p - 1) * cs + col];
                    int p0 = plane[row_p * cs + col];
                    int q0 = plane[row_q * cs + col];
                    int q1 = plane[(row_q + 1) * cs + col];
                    int delta = ((q0 - p0) * 4 + p1 - q1 + 4) >> 3;
                    int p0n, q0n;
                    if (delta > tc) delta = tc;
                    else if (delta < -tc) delta = -tc;
                    p0n = p0 + delta;
                    q0n = q0 - delta;
                    if (p0n < 0) p0n = 0;
                    else if (p0n > max_val) p0n = max_val;
                    if (q0n < 0) q0n = 0;
                    else if (q0n > max_val) q0n = max_val;
                    if (filter_p)
                        plane[row_p * cs + col] = (uint16_t)p0n;
                    if (filter_q)
                        plane[row_q * cs + col] = (uint16_t)q0n;
                }
            }
        }
    }
}

void heic_apply_deblock(heic_frame *frame, const uint8_t *flags, const int8_t *qp_map,
                        uint32_t deblock_stride,
                        int cb_qp_offset, int cr_qp_offset,
                        const heic_ctb_filter_info *filter_map,
                        uint32_t width_ctbs, uint32_t ctb_size,
                        int loop_filter_across_tiles,
                        const uint8_t *pred_mode,
                        const heic_pb_motion *mv_info, uint32_t pu_stride,
                        uint32_t min_pu, const uint8_t *cbf_map,
                        const int ref_poc[2][HEIC_MAX_REF_PICS],
                        const uint8_t *pcm_map)
{
    uint32_t w, h, x, y;
    int vert_edge_mask = HEIC_DEBLOCK_FLAG_VERT | HEIC_DEBLOCK_PB_EDGE_VERT;
    int horiz_edge_mask = HEIC_DEBLOCK_FLAG_HORIZ | HEIC_DEBLOCK_PB_EDGE_HORIZ;

    if (!frame || !flags || !qp_map || deblock_stride == 0) return;
    w = (uint32_t)frame->width;
    h = (uint32_t)frame->height;
    if (w == 0 || h == 0) return;

    /* Pass 1: vertical edges (x every 8, y every 4) */
    for (x = 8; x < w; x += 8) {
        for (y = 0; y < h; y += 4) {
            uint32_t bx = x / 4, by = y / 4;
            size_t idx = (size_t)by * deblock_stride + bx;
            int flags_v = flags[idx];
            int qp_q, qp_p, bs;
            const heic_ctb_filter_info *q_filter;
            if (!deblock_edge_allowed(
                    filter_map, width_ctbs, ctb_size,
                    loop_filter_across_tiles, x, y, 1, &q_filter))
                continue;
            if ((flags_v & vert_edge_mask) == 0) continue;
            qp_q = (int)qp_map[idx];
            qp_p = bx > 0 ? (int)qp_map[(size_t)by * deblock_stride + (bx - 1)] : qp_q;
            bs = compute_bs(x, y, 1,
                            (flags_v & HEIC_DEBLOCK_FLAG_VERT) != 0,
                            pred_mode, mv_info, pu_stride, min_pu, cbf_map,
                            deblock_stride, ref_poc);
            if (bs > 0)
                filter_edge_luma(
                    frame, x, y, 1, qp_p, qp_q,
                    q_filter ? q_filter->beta_offset : 0,
                    q_filter ? q_filter->tc_offset : 0, bs,
                    !pcm_at(pcm_map, deblock_stride, x - 1, y),
                    !pcm_at(pcm_map, deblock_stride, x, y));
        }
    }

    /* Pass 2: horizontal edges (y every 8, x every 4) */
    for (y = 8; y < h; y += 8) {
        for (x = 0; x < w; x += 4) {
            uint32_t bx = x / 4, by = y / 4;
            size_t idx = (size_t)by * deblock_stride + bx;
            int flags_h = flags[idx];
            int qp_q, qp_p, bs;
            const heic_ctb_filter_info *q_filter;
            if (!deblock_edge_allowed(
                    filter_map, width_ctbs, ctb_size,
                    loop_filter_across_tiles, x, y, 0, &q_filter))
                continue;
            if ((flags_h & horiz_edge_mask) == 0) continue;
            qp_q = (int)qp_map[idx];
            qp_p = by > 0 ? (int)qp_map[(size_t)(by - 1) * deblock_stride + bx] : qp_q;
            bs = compute_bs(x, y, 0,
                            (flags_h & HEIC_DEBLOCK_FLAG_HORIZ) != 0,
                            pred_mode, mv_info, pu_stride, min_pu, cbf_map,
                            deblock_stride, ref_poc);
            if (bs > 0)
                filter_edge_luma(
                    frame, x, y, 0, qp_p, qp_q,
                    q_filter ? q_filter->beta_offset : 0,
                    q_filter ? q_filter->tc_offset : 0, bs,
                    !pcm_at(pcm_map, deblock_stride, x, y - 1),
                    !pcm_at(pcm_map, deblock_stride, x, y));
        }
    }

    apply_chroma_deblocking(
        frame, flags, qp_map, deblock_stride,
        cb_qp_offset, cr_qp_offset, filter_map, width_ctbs, ctb_size,
        loop_filter_across_tiles, pred_mode, mv_info, pu_stride, min_pu,
        cbf_map, ref_poc, pcm_map);
}

void heic_mark_tu_boundary(uint8_t *flags, uint32_t deblock_stride, uint32_t map_n,
                           uint32_t x, uint32_t y, uint32_t size)
{
    uint32_t bx = x / 4, by = y / 4, bs = size / 4, j, i;
    if (!flags || deblock_stride == 0 || size < 4) return;
    if (x > 0) {
        for (j = 0; j < bs; j++) {
            size_t idx = (size_t)(by + j) * deblock_stride + bx;
            if (idx < map_n) flags[idx] |= HEIC_DEBLOCK_FLAG_VERT;
        }
    }
    if (y > 0) {
        for (i = 0; i < bs; i++) {
            size_t idx = (size_t)by * deblock_stride + bx + i;
            if (idx < map_n) flags[idx] |= HEIC_DEBLOCK_FLAG_HORIZ;
        }
    }
}

void heic_store_deblock_qp(int8_t *qp_map, uint32_t deblock_stride, uint32_t map_n,
                           uint32_t x, uint32_t y, uint32_t size, int8_t qp)
{
    uint32_t bx = x / 4, by = y / 4, bs = size / 4, j, i;
    if (!qp_map || deblock_stride == 0 || size < 4) return;
    for (j = 0; j < bs; j++) {
        for (i = 0; i < bs; i++) {
            size_t idx = (size_t)(by + j) * deblock_stride + bx + i;
            if (idx < map_n) qp_map[idx] = qp;
        }
    }
}

void heic_mark_pb_boundary(uint8_t *flags, uint32_t deblock_stride, uint32_t map_n,
                           uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                           int vertical)
{
    uint32_t bx, by, bs, j, i;
    if (!flags || deblock_stride == 0) return;
    if (vertical) {
        if (x == 0 || height < 4) return;
        bx = x / 4;
        by = y / 4;
        bs = height / 4;
        for (j = 0; j < bs; j++) {
            size_t idx = (size_t)(by + j) * deblock_stride + bx;
            if (idx < map_n) flags[idx] |= HEIC_DEBLOCK_PB_EDGE_VERT;
        }
    } else {
        if (y == 0 || width < 4) return;
        bx = x / 4;
        by = y / 4;
        bs = width / 4;
        for (i = 0; i < bs; i++) {
            size_t idx = (size_t)by * deblock_stride + bx + i;
            if (idx < map_n) flags[idx] |= HEIC_DEBLOCK_PB_EDGE_HORIZ;
        }
    }
}
