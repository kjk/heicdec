/* hevc_inter.c -- scalar HEVC motion compensation (uni/bi-pred via 14-bit
   intermediates) and weighted prediction */
#include "heic_internal.h"

static const int16_t LUMA_FILTER[4][8] = {
    {0, 0, 0, 64, 0, 0, 0, 0},
    {-1, 4, -10, 58, 17, -5, 1, 0},
    {-1, 4, -11, 40, 40, -11, 4, -1},
    {0, 1, -5, 17, 58, -10, 4, -1}
};

static const int16_t CHROMA_FILTER[8][4] = {
    {0, 64, 0, 0},
    {-2, 58, 10, -2},
    {-4, 54, 16, -2},
    {-6, 46, 28, -4},
    {-4, 36, 36, -4},
    {-4, 28, 46, -6},
    {-2, 16, 54, -4},
    {-2, 10, 58, -2}
};

static int clip_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Largest block a filter reads: 64 samples plus 7 taps of margin. */
#define MC_MAX_BLOCK (64 + 7)

/* The (bw x bh) reference samples whose top-left is (x0, y0) in `plane`.
   A block fully inside the picture is read in place. Otherwise it is copied
   into `tmp` with coordinates clamped to the picture (the reference padding
   of H.265 8.5.3.3.3), so the filters never clamp per tap. */
static const uint16_t *mc_ref_block(const uint16_t *plane, int stride, int pw,
                                    int ph, int x0, int y0, int bw, int bh,
                                    uint16_t *tmp, int *out_stride)
{
    int i, j;
    if (x0 >= 0 && y0 >= 0 && x0 + bw <= pw && y0 + bh <= ph) {
        *out_stride = stride;
        return plane + (size_t)y0 * (size_t)stride + (size_t)x0;
    }
    for (j = 0; j < bh; j++) {
        const uint16_t *row =
            plane + (size_t)clip_i(y0 + j, 0, ph - 1) * (size_t)stride;
        uint16_t *d = tmp + j * bw;
        for (i = 0; i < bw; i++) d[i] = row[clip_i(x0 + i, 0, pw - 1)];
    }
    *out_stride = bw;
    return tmp;
}

/* Prediction samples before the final rounding ("internal precision") have
   14 bits plus sign up to 12-bit video and are kept in int16. Deeper video
   needs more (a 16-bit sample << 2 is 18 bits), so the caller then passes an
   int32 buffer (`wide`). */
#define MC_STORE(idx, v)                                 \
    do {                                                 \
        if (out32) out32[idx] = (int32_t)(v);            \
        else out[idx] = (int16_t)(v);                    \
    } while (0)

/* The fractional cases through the SIMD kernel: `p` is the reference block
   from mc_ref_block (starting taps/2 - 1 samples before the block in each
   filtered direction). Only up to 12 bits, where the samples and the
   first-pass values (at most 4095 * 112 >> 4) fit in int16. Returns 0 when
   the scalar code has to do it. */
static int mc_filter_simd(const uint16_t *p, int ps, int bit_depth,
                          const int16_t *fx, const int16_t *fy, int taps,
                          int frac_x, int frac_y, int w, int h, int shift1,
                          int16_t *out, uint32_t out_stride)
{
    int16_t mid[(64 + 7) * 64];
    if (bit_depth > 12 || w < 4) return 0;
    if (frac_y == 0)
        return heic_simd_mc_filter(p, 1, ps, fx, taps, w, h, shift1,
                                   out, (ptrdiff_t)out_stride);
    if (frac_x == 0)
        return heic_simd_mc_filter(p, ps, ps, fy, taps, w, h, shift1,
                                   out, (ptrdiff_t)out_stride);
    if (!heic_simd_mc_filter(p, 1, ps, fx, taps, w, h + taps - 1, shift1,
                             mid, w))
        return 0;
    return heic_simd_mc_filter(mid, w, w, fy, taps, w, h, 6,
                               out, (ptrdiff_t)out_stride);
}

int heic_mc_luma_internal(const heic_frame *ref, heic_mv mv,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          void *out_v, int wide, uint32_t out_stride,
                          int32_t *scratch, size_t scratch_n)
{
    int16_t *out = wide ? NULL : (int16_t *)out_v;
    int32_t *out32 = wide ? (int32_t *)out_v : NULL;
    uint16_t tmp[MC_MAX_BLOCK * MC_MAX_BLOCK];
    const uint16_t *p;
    int ps, int_x, int_y, frac_x, frac_y, shift1, shift3;
    uint32_t i, j;
    if (!ref || !ref->y || !out_v || !scratch || !w || !h ||
        w > 64 || h > 64 || out_stride < w)
        return -1;
    int_x = (int)x + ((int)mv.x >> 2);
    int_y = (int)y + ((int)mv.y >> 2);
    frac_x = (int)mv.x & 3;
    frac_y = (int)mv.y & 3;
    /* H.265 8.5.3.3.3: shift1 = Min(4, BitDepth - 8), shift3 =
       Max(2, 14 - BitDepth). */
    shift1 = ref->bit_depth - 8;
    if (shift1 > 4) shift1 = 4;
    shift3 = 14 - ref->bit_depth;
    if (shift3 < 2) shift3 = 2;

    /* 8-tap filter: 3 samples before, 4 after, in each filtered direction. */
    p = mc_ref_block(ref->y, ref->y_stride, ref->width, ref->height,
                     int_x - (frac_x ? 3 : 0), int_y - (frac_y ? 3 : 0),
                     (int)w + (frac_x ? 7 : 0), (int)h + (frac_y ? 7 : 0),
                     tmp, &ps);

    if (frac_x == 0 && frac_y == 0) {
        for (j = 0; j < h; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < w; i++) MC_STORE(orow + i, s[i] << shift3);
        }
    } else if (!wide
               && mc_filter_simd(p, ps, ref->bit_depth, LUMA_FILTER[frac_x],
                              LUMA_FILTER[frac_y], 8, frac_x, frac_y,
                              (int)w, (int)h, shift1, out, out_stride)) {
        /* done */
    } else if (frac_y == 0) {
        const int16_t *c = LUMA_FILTER[frac_x];
        for (j = 0; j < h; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < w; i++) {
                int sum = s[i] * c[0] + s[i + 1] * c[1] + s[i + 2] * c[2] +
                          s[i + 3] * c[3] + s[i + 4] * c[4] + s[i + 5] * c[5] +
                          s[i + 6] * c[6] + s[i + 7] * c[7];
                MC_STORE(orow + i, sum >> shift1);
            }
        }
    } else if (frac_x == 0) {
        const int16_t *c = LUMA_FILTER[frac_y];
        for (j = 0; j < h; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < w; i++) {
                int sum = s[i] * c[0] + s[i + ps] * c[1] + s[i + 2 * ps] * c[2] +
                          s[i + 3 * ps] * c[3] + s[i + 4 * ps] * c[4] +
                          s[i + 5 * ps] * c[5] + s[i + 6 * ps] * c[6] +
                          s[i + 7 * ps] * c[7];
                MC_STORE(orow + i, sum >> shift1);
            }
        }
    } else {
        const int16_t *cx = LUMA_FILTER[frac_x];
        const int16_t *cy = LUMA_FILTER[frac_y];
        uint32_t tmp_h = h + 7;
        if ((size_t)w * tmp_h > scratch_n) return -1;
        for (j = 0; j < tmp_h; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            int32_t *t = scratch + (size_t)j * w;
            for (i = 0; i < w; i++) {
                int sum = s[i] * cx[0] + s[i + 1] * cx[1] + s[i + 2] * cx[2] +
                          s[i + 3] * cx[3] + s[i + 4] * cx[4] +
                          s[i + 5] * cx[5] + s[i + 6] * cx[6] + s[i + 7] * cx[7];
                t[i] = sum >> shift1;
            }
        }
        for (j = 0; j < h; j++) {
            const int32_t *t = scratch + (size_t)j * w;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < w; i++) {
                int64_t sum = 0;
                int k;
                for (k = 0; k < 8; k++)
                    sum += (int64_t)t[(size_t)k * w + i] * cy[k];
                MC_STORE(orow + i, sum >> 6);
            }
        }
    }
    return 0;
}

static int mc_chroma_plane_internal(const heic_frame *ref,
                                    const uint16_t *src, heic_mv mv,
                                    uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h,
                                    int sub_x, int sub_y,
                                    void *out_v, int wide, uint32_t out_stride,
                                    int32_t *scratch, size_t scratch_n)
{
    int16_t *out = wide ? NULL : (int16_t *)out_v;
    int32_t *out32 = wide ? (int32_t *)out_v : NULL;
    uint16_t tmp[MC_MAX_BLOCK * MC_MAX_BLOCK];
    const uint16_t *p;
    int ps;
    uint32_t cx = x / (uint32_t)sub_x, cy = y / (uint32_t)sub_y;
    uint32_t cw = w / (uint32_t)sub_x, ch = h / (uint32_t)sub_y;
    int cmv_x = sub_x > 1 ? mv.x : (int)mv.x * 2;
    int cmv_y = sub_y > 1 ? mv.y : (int)mv.y * 2;
    int int_x = (int)cx + (cmv_x >> 3);
    int int_y = (int)cy + (cmv_y >> 3);
    int frac_x = cmv_x & 7, frac_y = cmv_y & 7;
    int shift1 = ref->chroma_bit_depth > 12 ? 4 : ref->chroma_bit_depth - 8;
    int shift3 = 14 - ref->chroma_bit_depth;
    uint32_t i, j;
    if (!cw || !ch) return 0;
    if (!src || !out_v || out_stride < cw || cw > 64 || ch > 64) return -1;
    if (shift3 < 2) shift3 = 2;

    /* 4-tap filter: 1 sample before, 2 after, in each filtered direction. */
    p = mc_ref_block(src, ref->c_stride, ref->c_width, ref->c_height,
                     int_x - (frac_x ? 1 : 0), int_y - (frac_y ? 1 : 0),
                     (int)cw + (frac_x ? 3 : 0), (int)ch + (frac_y ? 3 : 0),
                     tmp, &ps);

    if (frac_x == 0 && frac_y == 0) {
        for (j = 0; j < ch; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < cw; i++) MC_STORE(orow + i, s[i] << shift3);
        }
    } else if (!wide
               && mc_filter_simd(p, ps, ref->chroma_bit_depth,
                              CHROMA_FILTER[frac_x], CHROMA_FILTER[frac_y], 4,
                              frac_x, frac_y, (int)cw, (int)ch, shift1,
                              out, out_stride)) {
        /* done */
    } else if (frac_y == 0) {
        const int16_t *c = CHROMA_FILTER[frac_x];
        for (j = 0; j < ch; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < cw; i++) {
                int sum = s[i] * c[0] + s[i + 1] * c[1] + s[i + 2] * c[2] +
                          s[i + 3] * c[3];
                MC_STORE(orow + i, sum >> shift1);
            }
        }
    } else if (frac_x == 0) {
        const int16_t *c = CHROMA_FILTER[frac_y];
        for (j = 0; j < ch; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < cw; i++) {
                int sum = s[i] * c[0] + s[i + ps] * c[1] + s[i + 2 * ps] * c[2] +
                          s[i + 3 * ps] * c[3];
                MC_STORE(orow + i, sum >> shift1);
            }
        }
    } else {
        const int16_t *fx = CHROMA_FILTER[frac_x];
        const int16_t *fy = CHROMA_FILTER[frac_y];
        uint32_t tmp_h = ch + 3;
        if ((size_t)cw * tmp_h > scratch_n) return -1;
        for (j = 0; j < tmp_h; j++) {
            const uint16_t *s = p + (size_t)j * (size_t)ps;
            int32_t *t = scratch + (size_t)j * cw;
            for (i = 0; i < cw; i++) {
                int sum = s[i] * fx[0] + s[i + 1] * fx[1] + s[i + 2] * fx[2] +
                          s[i + 3] * fx[3];
                t[i] = sum >> shift1;
            }
        }
        for (j = 0; j < ch; j++) {
            const int32_t *t = scratch + (size_t)j * cw;
            size_t orow = (size_t)j * out_stride;
            for (i = 0; i < cw; i++) {
                int64_t sum = 0;
                int k;
                for (k = 0; k < 4; k++)
                    sum += (int64_t)t[(size_t)k * cw + i] * fy[k];
                MC_STORE(orow + i, sum >> 6);
            }
        }
    }
    return 0;
}

int heic_mc_chroma_internal(const heic_frame *ref, heic_mv mv,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            void *out_cb, void *out_cr, int wide,
                            uint32_t out_stride,
                            int32_t *scratch, size_t scratch_n)
{
    int sub_x, sub_y;
    if (!ref || ref->chroma_format == 0) return 0;
    if (!ref->cb || !ref->cr || !out_cb || !out_cr) return -1;
    sub_x = ref->chroma_format == 3 ? 1 : 2;
    sub_y = ref->chroma_format == 1 ? 2 : 1;
    if (mc_chroma_plane_internal(ref, ref->cb, mv, x, y, w, h,
                                 sub_x, sub_y, out_cb, wide, out_stride,
                                 scratch, scratch_n) != 0)
        return -1;
    return mc_chroma_plane_internal(ref, ref->cr, mv, x, y, w, h,
                                    sub_x, sub_y, out_cr, wide, out_stride,
                                    scratch, scratch_n);
}
