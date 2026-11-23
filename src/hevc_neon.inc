/* AArch64 kernels included by hevc_simd.c after the shared IDCT. */

static inline int32x4_t hn_clamp(int32x4_t v, int lo, int hi)
{
    return vminq_s32(vmaxq_s32(v, vdupq_n_s32(lo)), vdupq_n_s32(hi));
}

static inline int32x4_t hn_widen_u16(uint16x4_t v)
{
    return vreinterpretq_s32_u32(vmovl_u16(v));
}

int heic_simd_add_residual(uint16_t *plane, int stride, int x0, int y0,
                           const int16_t *residual, int size, int max_val)
{
    uint16x8_t max8 = vdupq_n_u16((uint16_t)max_val);
    int y, x;
    if (size < 4 || (size & 3) || max_val > 65535) return 0;
    for (y = 0; y < size; y++) {
        uint16_t *dst = plane + (size_t)(y0 + y) * (size_t)stride + (size_t)x0;
        const int16_t *src = residual + y * size;
        for (x = 0; x + 8 <= size; x += 8) {
            uint16x8_t d = vld1q_u16(dst + x);
            int16x8_t r = vld1q_s16(src + x);
            /* Apply positive and negative changes with unsigned saturation.
               Negate as unsigned so a residual of -32768 works too. */
            uint16x8_t positive = vreinterpretq_u16_s16(vmaxq_s16(r, vdupq_n_s16(0)));
            uint16x8_t negative = vreinterpretq_u16_s16(vminq_s16(r, vdupq_n_s16(0)));
            d = vqaddq_u16(d, positive);
            d = vqsubq_u16(d, vsubq_u16(vdupq_n_u16(0), negative));
            vst1q_u16(dst + x, vminq_u16(d, max8));
        }
        if (x < size) {
            int32x4_t sum = vaddw_s16(hn_widen_u16(vld1_u16(dst + x)), vld1_s16(src + x));
            vst1_u16(dst + x, vmin_u16(vqmovun_s32(sum), vget_low_u16(max8)));
        }
    }
    return 1;
}

int heic_simd_dequant(int16_t *coeffs, int n, int32_t combined, int shift)
{
    int32x4_t sh = vdupq_n_s32(-shift);
    int i;
    if (n < 8 || (n & 7) || shift < 0 || combined > 65536) return 0;
    for (i = 0; i < n; i += 8) {
        int16x8_t v = vld1q_s16(coeffs + i);
        int32x4_t lo, hi;
        if (vmaxvq_u16(vreinterpretq_u16_s16(v)) == 0) continue;
        lo = vmulq_n_s32(vmovl_s16(vget_low_s16(v)), combined);
        hi = vmulq_n_s32(vmovl_high_s16(v), combined);
        vst1q_s16(coeffs + i, vqmovn_high_s32(vqmovn_s32(vrshlq_s32(lo, sh)),
                                              vrshlq_s32(hi, sh)));
    }
    return 1;
}

int heic_simd_intra_ang_row(uint16_t *dst, const int32_t *ref, int n,
                            int a, int b, int max_val)
{
    uint16x4_t mx = vdup_n_u16((uint16_t)max_val);
    int i;
    if (n < 4 || (n & 3)) return 0;
    for (i = 0; i < n; i += 4) {
        int32x4_t v = vmulq_n_s32(vld1q_s32(ref + i), a);
        v = vmlaq_n_s32(v, vld1q_s32(ref + i + 1), b);
        vst1_u16(dst + i, vmin_u16(vqrshrun_n_s32(v, 5), mx));
    }
    return 1;
}

int heic_simd_u16_to_i32_avail(const uint16_t *src, int32_t *border, int *avail, int n)
{
    int i;
    if (n < 4) return 0;
    for (i = 0; i + 4 <= n; i += 4) {
        vst1q_s32(border + i, hn_widen_u16(vld1_u16(src + i)));
        vst1q_s32(avail + i, vdupq_n_s32(1));
    }
    for (; i < n; i++) {
        border[i] = src[i];
        avail[i] = 1;
    }
    return 1;
}

typedef struct {
    int32x4_t r, g, b;
} hn_rgb4;

static inline hn_rgb4 hn_chroma_contrib(int16x4_t cb, int16x4_t cr, int full,
                                        int16_t kr, int16_t kgb, int16_t kgr, int16_t kb)
{
    hn_rgb4 c;
    if (!full) {
        cb = vmin_s16(vmax_s16(cb, vdup_n_s16(16)), vdup_n_s16(240));
        cr = vmin_s16(vmax_s16(cr, vdup_n_s16(16)), vdup_n_s16(240));
    }
    cb = vsub_s16(cb, vdup_n_s16(128));
    cr = vsub_s16(cr, vdup_n_s16(128));
    c.r = vmull_n_s16(cr, kr);
    c.g = vmlal_n_s16(vmull_n_s16(cb, kgb), cr, kgr);
    c.b = vmull_n_s16(cb, kb);
    if (full) {
        c.r = vrshrq_n_s32(c.r, 8);
        c.g = vrshrq_n_s32(c.g, 8);
        c.b = vrshrq_n_s32(c.b, 8);
    }
    return c;
}

static inline int16x4_t hn_color_samples(uint16x4_t v)
{
    return vreinterpret_s16_u16(vand_u16(v, vdup_n_u16(255)));
}

static inline hn_rgb4 hn_add_luma(const uint16_t *yp, hn_rgb4 chroma, int full, int16_t ky)
{
    int16x4_t y = hn_color_samples(vld1_u16(yp));
    int32x4_t luma;
    hn_rgb4 rgb;
    if (full) {
        luma = vmovl_s16(y);
        rgb.r = vaddq_s32(luma, chroma.r);
        rgb.g = vaddq_s32(luma, chroma.g);
        rgb.b = vaddq_s32(luma, chroma.b);
    } else {
        y = vmin_s16(vmax_s16(y, vdup_n_s16(16)), vdup_n_s16(235));
        luma = vmull_n_s16(vsub_s16(y, vdup_n_s16(16)), ky);
        rgb.r = vrshrq_n_s32(vaddq_s32(luma, chroma.r), 13);
        rgb.g = vrshrq_n_s32(vaddq_s32(luma, chroma.g), 13);
        rgb.b = vrshrq_n_s32(vaddq_s32(luma, chroma.b), 13);
    }
    return rgb;
}

static inline void hn_store_rgb8(uint8_t *dst, hn_rgb4 lo, hn_rgb4 hi)
{
    uint8x8x3_t rgb;
    rgb.val[0] = vqmovn_u16(vqmovun_high_s32(vqmovun_s32(lo.r), hi.r));
    rgb.val[1] = vqmovn_u16(vqmovun_high_s32(vqmovun_s32(lo.g), hi.g));
    rgb.val[2] = vqmovn_u16(vqmovun_high_s32(vqmovun_s32(lo.b), hi.b));
    vst3_u8(dst, rgb);
}

static inline int hn_ycc_rows(const uint16_t *yp0, const uint16_t *yp1,
                               const uint16_t *cbp, const uint16_t *crp,
                               uint8_t *row0, uint8_t *row1, int w, int subsampled,
                               int phase, int full, const int32_t yv[256],
                               const int32_t cr_r[256], const int32_t cb_g[256],
                               const int32_t cr_g[256], const int32_t cb_b[256])
{
    int16_t kr = (int16_t)cr_r[129], kgb = (int16_t)cb_g[129];
    int16_t kgr = (int16_t)cr_g[129], kb = (int16_t)cb_b[129];
    int16_t ky = (int16_t)yv[17];
    int x = 0;
    if (w < 8) return 0;
    /* Peel an odd crop column so each subsequent 4:2:0 vector reads exactly
       four chroma samples for eight luma samples. */
    if (phase) {
        ycc_rgb_tail(yp0, cbp, crp, row0, 0, 1, subsampled, phase, full, yv,
                     cr_r, cb_g, cr_g, cb_b);
        if (row1)
            ycc_rgb_tail(yp1, cbp, crp, row1, 0, 1, subsampled, phase, full, yv,
                         cr_r, cb_g, cr_g, cb_b);
        x = 1;
    }
    for (; x + 8 <= w; x += 8) {
        int cx = subsampled ? (x + phase) >> 1 : x;
        int16x4_t cb0 = hn_color_samples(vld1_u16(cbp + cx));
        int16x4_t cr0 = hn_color_samples(vld1_u16(crp + cx));
        int16x4_t cb1, cr1;
        hn_rgb4 lo, hi;
        if (subsampled) {
            cb1 = vzip2_s16(cb0, cb0);
            cr1 = vzip2_s16(cr0, cr0);
            cb0 = vzip1_s16(cb0, cb0);
            cr0 = vzip1_s16(cr0, cr0);
        } else {
            cb1 = hn_color_samples(vld1_u16(cbp + cx + 4));
            cr1 = hn_color_samples(vld1_u16(crp + cx + 4));
        }
        lo = hn_chroma_contrib(cb0, cr0, full, kr, kgb, kgr, kb);
        hi = hn_chroma_contrib(cb1, cr1, full, kr, kgb, kgr, kb);
        hn_store_rgb8(row0 + 3 * x, hn_add_luma(yp0 + x, lo, full, ky),
                      hn_add_luma(yp0 + x + 4, hi, full, ky));
        if (row1)
            hn_store_rgb8(row1 + 3 * x, hn_add_luma(yp1 + x, lo, full, ky),
                          hn_add_luma(yp1 + x + 4, hi, full, ky));
    }
    ycc_rgb_tail(yp0, cbp, crp, row0, x, w, subsampled, phase, full, yv,
                 cr_r, cb_g, cr_g, cb_b);
    if (row1)
        ycc_rgb_tail(yp1, cbp, crp, row1, x, w, subsampled, phase, full, yv,
                     cr_r, cb_g, cr_g, cb_b);
    return 1;
}

int heic_simd_ycc_444_row(const uint16_t *yp, const uint16_t *cbp, const uint16_t *crp,
                          uint8_t *row, int w, int full, const int32_t yv[256],
                          const int32_t cr_r[256], const int32_t cb_g[256],
                          const int32_t cr_g[256], const int32_t cb_b[256])
{
    return hn_ycc_rows(yp, NULL, cbp, crp, row, NULL, w, 0, 0, full,
                       yv, cr_r, cb_g, cr_g, cb_b);
}

int heic_simd_ycc_420_row(const uint16_t *yp, const uint16_t *cbp, const uint16_t *crp,
                          uint8_t *row, int w, int phase, int full, const int32_t yv[256],
                          const int32_t cr_r[256], const int32_t cb_g[256],
                          const int32_t cr_g[256], const int32_t cb_b[256])
{
    return hn_ycc_rows(yp, NULL, cbp, crp, row, NULL, w, 1, phase, full,
                       yv, cr_r, cb_g, cr_g, cb_b);
}

int heic_simd_ycc_420_2rows(const uint16_t *yp0, const uint16_t *yp1,
                            const uint16_t *cbp, const uint16_t *crp,
                            uint8_t *row0, uint8_t *row1, int w, int phase,
                            int full, const int32_t yv[256], const int32_t cr_r[256],
                            const int32_t cb_g[256], const int32_t cr_g[256],
                            const int32_t cb_b[256])
{
    if (!yp1 || !row1) return 0;
    return hn_ycc_rows(yp0, yp1, cbp, crp, row0, row1, w, 1, phase, full,
                       yv, cr_r, cb_g, cr_g, cb_b);
}

static inline int32x4_t hn_load_edge(const uint16_t *plane, size_t base, size_t along)
{
    const uint16_t *p = plane + base;
    if (along == 1) return hn_widen_u16(vld1_u16(p));
    return (int32x4_t){p[0], p[along], p[2 * along], p[3 * along]};
}

static inline void hn_store_edge(uint16_t *plane, size_t base, size_t along, int32x4_t v)
{
    uint16_t *p = plane + base;
    uint16x4_t u = vmovn_u32(vreinterpretq_u32_s32(v));
    if (along == 1) {
        vst1_u16(p, u);
    } else {
        vst1_lane_u16(p, u, 0);
        vst1_lane_u16(p + along, u, 1);
        vst1_lane_u16(p + 2 * along, u, 2);
        vst1_lane_u16(p + 3 * along, u, 3);
    }
}

int heic_simd_chroma_edge4(uint16_t *plane, int stride, size_t base_q0, int across,
                           int tc, int max_val, int along_is_stride)
{
    size_t along = along_is_stride ? (size_t)stride : 1;
    size_t ac = (size_t)across;
    int32x4_t p0, p1, q0, q1, delta;
    if (tc <= 0) return 0;
    p0 = hn_load_edge(plane, base_q0 - ac, along);
    p1 = hn_load_edge(plane, base_q0 - 2 * ac, along);
    q0 = hn_load_edge(plane, base_q0, along);
    q1 = hn_load_edge(plane, base_q0 + ac, along);
    delta = vaddq_s32(vshlq_n_s32(vsubq_s32(q0, p0), 2), vsubq_s32(p1, q1));
    delta = hn_clamp(vrshrq_n_s32(delta, 3), -tc, tc);
    hn_store_edge(plane, base_q0 - ac, along, hn_clamp(vaddq_s32(p0, delta), 0, max_val));
    hn_store_edge(plane, base_q0, along, hn_clamp(vsubq_s32(q0, delta), 0, max_val));
    return 1;
}

static inline int32x4_t hn_strong_clip(int32x4_t filtered, int32x4_t original,
                                       int tc2, int max_val)
{
    int32x4_t delta = hn_clamp(vsubq_s32(filtered, original), -tc2, tc2);
    return hn_clamp(vaddq_s32(original, delta), 0, max_val);
}

static inline void hn_luma_strong_side(uint16_t *plane, size_t p0_pos, size_t p1_pos,
                                       size_t p2_pos, size_t along, int tc2, int max_val,
                                       int32x4_t p0, int32x4_t p1, int32x4_t p2,
                                       int32x4_t p3, int32x4_t q0, int32x4_t q1)
{
    int32x4_t sum = vaddq_s32(p0, q0);
    int32x4_t f0 = vaddq_s32(vaddq_s32(p2, q1),
                             vshlq_n_s32(vaddq_s32(sum, p1), 1));
    int32x4_t f1 = vaddq_s32(sum, vaddq_s32(p1, p2));
    int32x4_t f2 = vmlaq_n_s32(vaddq_s32(sum, vaddq_s32(p1, vshlq_n_s32(p3, 1))),
                              p2, 3);
    hn_store_edge(plane, p0_pos, along, hn_strong_clip(vrshrq_n_s32(f0, 3), p0, tc2, max_val));
    hn_store_edge(plane, p1_pos, along, hn_strong_clip(vrshrq_n_s32(f1, 2), p1, tc2, max_val));
    hn_store_edge(plane, p2_pos, along, hn_strong_clip(vrshrq_n_s32(f2, 3), p2, tc2, max_val));
}

static inline int32x4_t hn_luma_weak_sample(int32x4_t p, int32x4_t delta,
                                           uint32x4_t mask, int max_val)
{
    return vbslq_s32(mask, hn_clamp(vaddq_s32(p, delta), 0, max_val), p);
}

int heic_simd_luma_filter4(uint16_t *plane, size_t base_p, size_t base_q,
                           size_t step_along, size_t step_across, int strong,
                           int d_ep, int d_eq, int tc, int max_val)
{
    size_t p1_pos = base_p - step_across, p2_pos = base_p - 2 * step_across;
    size_t q1_pos = base_q + step_across, q2_pos = base_q + 2 * step_across;
    int32x4_t p0, p1, p2, q0, q1, q2;
    if (tc <= 0) return 0;
    p0 = hn_load_edge(plane, base_p, step_along);
    p1 = hn_load_edge(plane, p1_pos, step_along);
    q0 = hn_load_edge(plane, base_q, step_along);
    q1 = hn_load_edge(plane, q1_pos, step_along);

    if (strong) {
        int32x4_t p3 = hn_load_edge(plane, p2_pos - step_across, step_along);
        int32x4_t q3 = hn_load_edge(plane, q2_pos + step_across, step_along);
        p2 = hn_load_edge(plane, p2_pos, step_along);
        q2 = hn_load_edge(plane, q2_pos, step_along);
        hn_luma_strong_side(plane, base_p, p1_pos, p2_pos, step_along, 2 * tc, max_val,
                            p0, p1, p2, p3, q0, q1);
        hn_luma_strong_side(plane, base_q, q1_pos, q2_pos, step_along, 2 * tc, max_val,
                            q0, q1, q2, q3, p0, p1);
    } else {
        int32x4_t delta = vmulq_n_s32(vsubq_s32(q0, p0), 9);
        uint32x4_t mask;
        delta = vrshrq_n_s32(vmlsq_n_s32(delta, vsubq_s32(q1, p1), 3), 4);
        mask = vcltq_s32(vabsq_s32(delta), vdupq_n_s32(10 * tc));
        delta = hn_clamp(delta, -tc, tc);
        hn_store_edge(plane, base_p, step_along, hn_luma_weak_sample(p0, delta, mask, max_val));
        hn_store_edge(plane, base_q, step_along,
                      hn_luma_weak_sample(q0, vnegq_s32(delta), mask, max_val));
        if (d_ep) {
            int32x4_t dp;
            p2 = hn_load_edge(plane, p2_pos, step_along);
            dp = vsubq_s32(vrhaddq_s32(p2, p0), p1);
            dp = hn_clamp(vshrq_n_s32(vaddq_s32(dp, delta), 1), -(tc >> 1), tc >> 1);
            hn_store_edge(plane, p1_pos, step_along, hn_luma_weak_sample(p1, dp, mask, max_val));
        }
        if (d_eq) {
            int32x4_t dq;
            q2 = hn_load_edge(plane, q2_pos, step_along);
            dq = vsubq_s32(vrhaddq_s32(q2, q0), q1);
            dq = hn_clamp(vshrq_n_s32(vsubq_s32(dq, delta), 1), -(tc >> 1), tc >> 1);
            hn_store_edge(plane, q1_pos, step_along, hn_luma_weak_sample(q1, dq, mask, max_val));
        }
    }
    return 1;
}

static inline uint16x8_t hn_sao_sign(uint16x8_t s, uint16x8_t n)
{
    /* Unsigned lane arithmetic holds either +1, 0, or -1 modulo 65536. */
    return vsubq_u16(vcltq_u16(s, n), vcgtq_u16(s, n));
}

static inline int hn_sao_edge_row(const uint16_t *srow, const uint16_t *n0,
                                   const uint16_t *n1, uint16_t *drow, int count,
                                   const int offset_table[5], int max_val)
{
    uint8_t table[16] = {0};
    uint8x16_t lut;
    uint16x8_t mx = vdupq_n_u16((uint16_t)max_val);
    int x, k;
    if (count < 8) return 0;
    /* Two lookups reconstruct each signed 16-bit offset. */
    for (k = 0; k < 5; k++) {
        uint16_t off = (uint16_t)offset_table[k];
        table[k] = (uint8_t)off;
        table[k + 8] = (uint8_t)(off >> 8);
    }
    lut = vld1q_u8(table);
    for (x = 0; x + 8 <= count; x += 8) {
        uint16x8_t s = vld1q_u16(srow + x);
        uint16x8_t signs = vaddq_u16(hn_sao_sign(s, vld1q_u16(n0 + x)),
                                     hn_sao_sign(s, vld1q_u16(n1 + x)));
        uint8x8_t idx = vmovn_u16(vaddq_u16(signs, vdupq_n_u16(2)));
        uint8x8_t lo = vqtbl1_u8(lut, idx);
        uint8x8_t hi = vqtbl1_u8(lut, vadd_u8(idx, vdup_n_u8(8)));
        uint16x8_t bits = vorrq_u16(vmovl_u8(lo), vshll_n_u8(hi, 8));
        int16x8_t off = vreinterpretq_s16_u16(bits);
        int32x4_t low = vaddw_s16(hn_widen_u16(vget_low_u16(s)), vget_low_s16(off));
        int32x4_t high = vaddw_high_s16(hn_widen_u16(vget_high_u16(s)), off);
        uint16x8_t v = vminq_u16(vqmovun_high_s32(vqmovun_s32(low), high), mx);
        vst1q_u16(drow + x, vbslq_u16(vceqzq_u16(bits), vld1q_u16(drow + x), v));
    }
    for (; x < count; x++) {
        int s = srow[x];
        int idx = 2 + (s > n0[x]) - (s < n0[x]) + (s > n1[x]) - (s < n1[x]);
        int off = offset_table[idx];
        if (off) {
            int v = s + off;
            drow[x] = (uint16_t)(v < 0 ? 0 : v > max_val ? max_val : v);
        }
    }
    return 1;
}

int heic_simd_sao_edge_h_row(const uint16_t *srow, uint16_t *drow, int x0, int x1,
                             const int offset_table[5], int max_val)
{
    if (x1 - x0 < 8) return 0;
    return hn_sao_edge_row(srow + x0, srow + x0 - 1, srow + x0 + 1, drow + x0,
                           x1 - x0, offset_table, max_val);
}

int heic_simd_sao_edge_v_row(const uint16_t *src, uint16_t *dst, int stride, int y,
                             int x0, int x1, const int offset_table[5], int max_val)
{
    size_t pos = (size_t)y * (size_t)stride + (size_t)x0;
    if (x1 - x0 < 8) return 0;
    return hn_sao_edge_row(src + pos, src + pos - stride, src + pos + stride, dst + pos,
                           x1 - x0, offset_table, max_val);
}

int heic_simd_mc_filter(const void *src, ptrdiff_t tap_step, ptrdiff_t row_step,
                        const int16_t *coef, int taps, int w, int h, int shift,
                        int16_t *out, ptrdiff_t out_stride)
{
    int32x4_t sh = vdupq_n_s32(-shift);
    int x, y, k;
    if ((taps != 4 && taps != 8) || w < 4) return 0;
    for (y = 0; y < h; y++) {
        const int16_t *s = (const int16_t *)src + y * row_step;
        int16_t *o = out + y * out_stride;
        for (x = 0; x + 8 <= w; x += 8) {
            int32x4_t lo = vdupq_n_s32(0), hi = vdupq_n_s32(0);
            for (k = 0; k < taps; k++) {
                int16x8_t v = vld1q_s16(s + x + k * tap_step);
                lo = vmlal_n_s16(lo, vget_low_s16(v), coef[k]);
                hi = vmlal_high_n_s16(hi, v, coef[k]);
            }
            vst1q_s16(o + x, vmovn_high_s32(vmovn_s32(vshlq_s32(lo, sh)), vshlq_s32(hi, sh)));
        }
        for (; x < w; x += 4) {
            int32x4_t sum = vdupq_n_s32(0);
            if (x + 4 > w) x = w - 4;
            for (k = 0; k < taps; k++)
                sum = vmlal_n_s16(sum, vld1_s16(s + x + k * tap_step), coef[k]);
            vst1_s16(o + x, vmovn_s32(vshlq_s32(sum, sh)));
        }
    }
    return 1;
}

int heic_simd_mc_put_row(const int16_t *a, const int16_t *b, int n, int shift,
                         int max_val, uint16_t *dst)
{
    int32x4_t sh = vdupq_n_s32(-shift);
    uint16x8_t mx = vdupq_n_u16((uint16_t)max_val);
    int i;
    if (n < 8 || shift < 1 || max_val > 65535) return 0;
    for (i = 0; i < n; i += 8) {
        int16x8_t av;
        int32x4_t lo, hi;
        if (i + 8 > n) i = n - 8;
        av = vld1q_s16(a + i);
        lo = vmovl_s16(vget_low_s16(av));
        hi = vmovl_high_s16(av);
        if (b) {
            int16x8_t bv = vld1q_s16(b + i);
            lo = vaddw_s16(lo, vget_low_s16(bv));
            hi = vaddw_high_s16(hi, bv);
        }
        vst1q_u16(dst + i, vminq_u16(mx, vqmovun_high_s32(vqmovun_s32(vrshlq_s32(lo, sh)),
                                                        vrshlq_s32(hi, sh))));
    }
    return 1;
}

int heic_simd_mc_filter_u16_wide(const uint16_t *src, ptrdiff_t tap_step,
                                 ptrdiff_t row_step, const int16_t *coef,
                                 int taps, int w, int h, int shift,
                                 int32_t *out, ptrdiff_t out_stride)
{
    int32x4_t sh = vdupq_n_s32(-shift);
    int x, y, k;
    if ((taps != 4 && taps != 8) || w < 4) return 0;
    for (y = 0; y < h; y++) {
        const uint16_t *s = src + y * row_step;
        int32_t *o = out + y * out_stride;
        for (x = 0; x < w; x += 4) {
            int32x4_t sum = vdupq_n_s32(0);
            if (x + 4 > w) x = w - 4;
            for (k = 0; k < taps; k++)
                sum = vmlaq_n_s32(sum, hn_widen_u16(vld1_u16(s + x + k * tap_step)), coef[k]);
            vst1q_s32(o + x, vshlq_s32(sum, sh));
        }
    }
    return 1;
}

int heic_simd_mc_filter_i32_wide(const int32_t *src, ptrdiff_t tap_step,
                                 ptrdiff_t row_step, const int16_t *coef,
                                 int taps, int w, int h, int shift,
                                 int32_t *out, ptrdiff_t out_stride)
{
    int32x4_t sh = vdupq_n_s32(-shift);
    int x, y, k;
    if ((taps != 4 && taps != 8) || w < 4) return 0;
    for (y = 0; y < h; y++) {
        const int32_t *s = src + y * row_step;
        int32_t *o = out + y * out_stride;
        for (x = 0; x < w; x += 4) {
            int32x4_t sum = vdupq_n_s32(0);
            if (x + 4 > w) x = w - 4;
            for (k = 0; k < taps; k++)
                sum = vmlaq_n_s32(sum, vld1q_s32(s + x + k * tap_step), coef[k]);
            vst1q_s32(o + x, vshlq_s32(sum, sh));
        }
    }
    return 1;
}

int heic_simd_mc_put_row_wide(const int32_t *a, const int32_t *b, int n,
                              int round, int shift, int max_val, uint16_t *dst)
{
    int32x4_t sh = vdupq_n_s32(-shift), rnd = vdupq_n_s32(round);
    uint16x4_t mx = vdup_n_u16((uint16_t)max_val);
    int i;
    if (n < 4) return 0;
    for (i = 0; i < n; i += 4) {
        int32x4_t v;
        if (i + 4 > n) i = n - 4;
        v = vld1q_s32(a + i);
        if (b) v = vaddq_s32(v, vld1q_s32(b + i));
        v = vshlq_s32(vaddq_s32(v, rnd), sh);
        vst1_u16(dst + i, vmin_u16(vqmovun_s32(v), mx));
    }
    return 1;
}

int heic_simd_mc_weight_row_wide(const int32_t *a, const int32_t *b, int n,
                                 int w0, int w1, int round, int shift,
                                 int offset, int max_val, uint16_t *dst)
{
    int32x4_t sh = vdupq_n_s32(-shift), rnd = vdupq_n_s32(round);
    int32x4_t off = vdupq_n_s32(offset);
    uint16x4_t mx = vdup_n_u16((uint16_t)max_val);
    int i;
    if (n < 4) return 0;
    for (i = 0; i < n; i += 4) {
        int32x4_t v;
        if (i + 4 > n) i = n - 4;
        v = vmulq_n_s32(vld1q_s32(a + i), w0);
        if (b) v = vmlaq_n_s32(v, vld1q_s32(b + i), w1);
        v = vaddq_s32(vshlq_s32(vaddq_s32(v, rnd), sh), off);
        vst1_u16(dst + i, vmin_u16(vqmovun_s32(v), mx));
    }
    return 1;
}
