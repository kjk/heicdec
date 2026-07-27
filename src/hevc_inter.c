/* hevc_inter.c -- scalar HEVC P-picture motion compensation */
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

static uint16_t clip_sample_i64(int64_t v, int max_val)
{
    if (v < 0) return 0;
    if (v > max_val) return (uint16_t)max_val;
    return (uint16_t)v;
}

int heic_mc_luma(const heic_frame *ref, heic_frame *dst, heic_mv mv,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 int32_t *scratch, size_t scratch_n)
{
    int int_x, int_y, frac_x, frac_y, shift1, max_val;
    uint32_t i, j;
    if (!ref || !dst || !ref->y || !dst->y || !scratch || !w || !h)
        return -1;
    if (w > 64 || h > 64 || x >= (uint32_t)dst->width ||
        y >= (uint32_t)dst->height)
        return -1;
    int_x = (int)x + ((int)mv.x >> 2);
    int_y = (int)y + ((int)mv.y >> 2);
    frac_x = (int)mv.x & 3;
    frac_y = (int)mv.y & 3;
    shift1 = ref->bit_depth - 8 + 6;
    max_val = (1 << ref->bit_depth) - 1;

    if (frac_x == 0 && frac_y == 0) {
        for (j = 0; j < h && y + j < (uint32_t)dst->height; j++)
            for (i = 0; i < w && x + i < (uint32_t)dst->width; i++) {
                int sx = clip_i(int_x + (int)i, 0, ref->width - 1);
                int sy = clip_i(int_y + (int)j, 0, ref->height - 1);
                dst->y[(y + j) * (uint32_t)dst->y_stride + x + i] =
                    ref->y[sy * ref->y_stride + sx];
            }
    } else if (frac_y == 0) {
        int offset = 1 << (shift1 - 1);
        for (j = 0; j < h && y + j < (uint32_t)dst->height; j++) {
            int sy = clip_i(int_y + (int)j, 0, ref->height - 1);
            for (i = 0; i < w && x + i < (uint32_t)dst->width; i++) {
                int sum = 0, k;
                for (k = 0; k < 8; k++) {
                    int sx = clip_i(int_x + (int)i + k - 3, 0, ref->width - 1);
                    sum += ref->y[sy * ref->y_stride + sx] *
                           LUMA_FILTER[frac_x][k];
                }
                dst->y[(y + j) * (uint32_t)dst->y_stride + x + i] =
                    clip_sample_i64((sum + offset) >> shift1, max_val);
            }
        }
    } else if (frac_x == 0) {
        int offset = 1 << (shift1 - 1);
        for (j = 0; j < h && y + j < (uint32_t)dst->height; j++)
            for (i = 0; i < w && x + i < (uint32_t)dst->width; i++) {
                int sum = 0, k;
                int sx = clip_i(int_x + (int)i, 0, ref->width - 1);
                for (k = 0; k < 8; k++) {
                    int sy = clip_i(int_y + (int)j + k - 3, 0, ref->height - 1);
                    sum += ref->y[sy * ref->y_stride + sx] *
                           LUMA_FILTER[frac_y][k];
                }
                dst->y[(y + j) * (uint32_t)dst->y_stride + x + i] =
                    clip_sample_i64((sum + offset) >> shift1, max_val);
            }
    } else {
        uint32_t tmp_h = h + 7;
        int total_shift = shift1 + 6;
        int64_t offset = (int64_t)1 << (total_shift - 1);
        if ((size_t)w * tmp_h > scratch_n) return -1;
        for (j = 0; j < tmp_h; j++) {
            int sy = clip_i(int_y + (int)j - 3, 0, ref->height - 1);
            for (i = 0; i < w; i++) {
                int sum = 0, k;
                for (k = 0; k < 8; k++) {
                    int sx = clip_i(int_x + (int)i + k - 3, 0, ref->width - 1);
                    sum += ref->y[sy * ref->y_stride + sx] *
                           LUMA_FILTER[frac_x][k];
                }
                scratch[j * w + i] = sum;
            }
        }
        for (j = 0; j < h && y + j < (uint32_t)dst->height; j++)
            for (i = 0; i < w && x + i < (uint32_t)dst->width; i++) {
                int64_t sum = 0;
                int k;
                for (k = 0; k < 8; k++)
                    sum += (int64_t)scratch[(j + (uint32_t)k) * w + i] *
                           LUMA_FILTER[frac_y][k];
                dst->y[(y + j) * (uint32_t)dst->y_stride + x + i] =
                    clip_sample_i64((sum + offset) >> total_shift, max_val);
            }
    }
    return 0;
}

int heic_mc_luma_internal(const heic_frame *ref, heic_mv mv,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          int16_t *out, uint32_t out_stride,
                          int32_t *scratch, size_t scratch_n)
{
    int int_x, int_y, frac_x, frac_y, shift1, shift3;
    uint32_t i, j;
    if (!ref || !ref->y || !out || !scratch || !w || !h ||
        w > 64 || h > 64 || out_stride < w)
        return -1;
    int_x = (int)x + ((int)mv.x >> 2);
    int_y = (int)y + ((int)mv.y >> 2);
    frac_x = (int)mv.x & 3;
    frac_y = (int)mv.y & 3;
    shift1 = ref->bit_depth - 8;
    shift3 = 14 - ref->bit_depth;
    if (shift3 < 2) shift3 = 2;

    if (frac_x == 0 && frac_y == 0) {
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                int sx = clip_i(int_x + (int)i, 0, ref->width - 1);
                int sy = clip_i(int_y + (int)j, 0, ref->height - 1);
                out[j * out_stride + i] =
                    (int16_t)(ref->y[sy * ref->y_stride + sx] << shift3);
            }
    } else if (frac_y == 0 || frac_x == 0) {
        const int16_t *coeff =
            LUMA_FILTER[frac_x ? frac_x : frac_y];
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                int sum = 0, k;
                for (k = 0; k < 8; k++) {
                    int sx = frac_x
                                 ? clip_i(int_x + (int)i + k - 3, 0,
                                          ref->width - 1)
                                 : clip_i(int_x + (int)i, 0, ref->width - 1);
                    int sy = frac_y
                                 ? clip_i(int_y + (int)j + k - 3, 0,
                                          ref->height - 1)
                                 : clip_i(int_y + (int)j, 0, ref->height - 1);
                    sum += ref->y[sy * ref->y_stride + sx] * coeff[k];
                }
                out[j * out_stride + i] = (int16_t)(sum >> shift1);
            }
    } else {
        uint32_t tmp_h = h + 7;
        if ((size_t)w * tmp_h > scratch_n) return -1;
        for (j = 0; j < tmp_h; j++) {
            int sy = clip_i(int_y + (int)j - 3, 0, ref->height - 1);
            for (i = 0; i < w; i++) {
                int sum = 0, k;
                for (k = 0; k < 8; k++) {
                    int sx = clip_i(int_x + (int)i + k - 3, 0,
                                    ref->width - 1);
                    sum += ref->y[sy * ref->y_stride + sx] *
                           LUMA_FILTER[frac_x][k];
                }
                scratch[j * w + i] = sum >> shift1;
            }
        }
        for (j = 0; j < h; j++)
            for (i = 0; i < w; i++) {
                int64_t sum = 0;
                int k;
                for (k = 0; k < 8; k++)
                    sum += (int64_t)scratch[(j + (uint32_t)k) * w + i] *
                           LUMA_FILTER[frac_y][k];
                out[j * out_stride + i] = (int16_t)(sum >> 6);
            }
    }
    return 0;
}

static int mc_chroma_plane(const heic_frame *ref, heic_frame *dst,
                           const uint16_t *src, uint16_t *out, heic_mv mv,
                           uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           int sub_x, int sub_y, int32_t *scratch,
                           size_t scratch_n)
{
    uint32_t cx = x / (uint32_t)sub_x, cy = y / (uint32_t)sub_y;
    uint32_t cw = w / (uint32_t)sub_x, ch = h / (uint32_t)sub_y;
    int cmv_x = sub_x > 1 ? mv.x : (int)mv.x * 2;
    int cmv_y = sub_y > 1 ? mv.y : (int)mv.y * 2;
    int int_x = (int)cx + (cmv_x >> 3);
    int int_y = (int)cy + (cmv_y >> 3);
    int frac_x = cmv_x & 7, frac_y = cmv_y & 7;
    int shift1 = ref->chroma_bit_depth - 8 + 6;
    int max_val = (1 << ref->chroma_bit_depth) - 1;
    uint32_t i, j;
    if (!cw || !ch || cx >= (uint32_t)dst->c_width ||
        cy >= (uint32_t)dst->c_height)
        return 0;

    if (frac_x == 0 && frac_y == 0) {
        for (j = 0; j < ch && cy + j < (uint32_t)dst->c_height; j++)
            for (i = 0; i < cw && cx + i < (uint32_t)dst->c_width; i++) {
                int sx = clip_i(int_x + (int)i, 0, ref->c_width - 1);
                int sy = clip_i(int_y + (int)j, 0, ref->c_height - 1);
                out[(cy + j) * (uint32_t)dst->c_stride + cx + i] =
                    src[sy * ref->c_stride + sx];
            }
    } else if (frac_y == 0 || frac_x == 0) {
        int offset = 1 << (shift1 - 1);
        for (j = 0; j < ch && cy + j < (uint32_t)dst->c_height; j++)
            for (i = 0; i < cw && cx + i < (uint32_t)dst->c_width; i++) {
                int sum = 0, k;
                for (k = 0; k < 4; k++) {
                    int sx = frac_x
                                 ? clip_i(int_x + (int)i + k - 1, 0,
                                          ref->c_width - 1)
                                 : clip_i(int_x + (int)i, 0, ref->c_width - 1);
                    int sy = frac_y
                                 ? clip_i(int_y + (int)j + k - 1, 0,
                                          ref->c_height - 1)
                                 : clip_i(int_y + (int)j, 0, ref->c_height - 1);
                    sum += src[sy * ref->c_stride + sx] *
                           (frac_x ? CHROMA_FILTER[frac_x][k]
                                   : CHROMA_FILTER[frac_y][k]);
                }
                out[(cy + j) * (uint32_t)dst->c_stride + cx + i] =
                    clip_sample_i64((sum + offset) >> shift1, max_val);
            }
    } else {
        uint32_t tmp_h = ch + 3;
        int total_shift = shift1 + 6;
        int64_t offset = (int64_t)1 << (total_shift - 1);
        if ((size_t)cw * tmp_h > scratch_n) return -1;
        for (j = 0; j < tmp_h; j++)
            for (i = 0; i < cw; i++) {
                int sum = 0, k;
                int sy = clip_i(int_y + (int)j - 1, 0, ref->c_height - 1);
                for (k = 0; k < 4; k++) {
                    int sx = clip_i(int_x + (int)i + k - 1, 0,
                                    ref->c_width - 1);
                    sum += src[sy * ref->c_stride + sx] *
                           CHROMA_FILTER[frac_x][k];
                }
                scratch[j * cw + i] = sum;
            }
        for (j = 0; j < ch && cy + j < (uint32_t)dst->c_height; j++)
            for (i = 0; i < cw && cx + i < (uint32_t)dst->c_width; i++) {
                int64_t sum = 0;
                int k;
                for (k = 0; k < 4; k++)
                    sum += (int64_t)scratch[(j + (uint32_t)k) * cw + i] *
                           CHROMA_FILTER[frac_y][k];
                out[(cy + j) * (uint32_t)dst->c_stride + cx + i] =
                    clip_sample_i64((sum + offset) >> total_shift, max_val);
            }
    }
    return 0;
}

static int mc_chroma_plane_internal(const heic_frame *ref,
                                    const uint16_t *src, heic_mv mv,
                                    uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h,
                                    int sub_x, int sub_y,
                                    int16_t *out, uint32_t out_stride,
                                    int32_t *scratch, size_t scratch_n)
{
    uint32_t cx = x / (uint32_t)sub_x, cy = y / (uint32_t)sub_y;
    uint32_t cw = w / (uint32_t)sub_x, ch = h / (uint32_t)sub_y;
    int cmv_x = sub_x > 1 ? mv.x : (int)mv.x * 2;
    int cmv_y = sub_y > 1 ? mv.y : (int)mv.y * 2;
    int int_x = (int)cx + (cmv_x >> 3);
    int int_y = (int)cy + (cmv_y >> 3);
    int frac_x = cmv_x & 7, frac_y = cmv_y & 7;
    int shift1 = ref->chroma_bit_depth - 8;
    int shift3 = 14 - ref->chroma_bit_depth;
    uint32_t i, j;
    if (!cw || !ch) return 0;
    if (!src || !out || out_stride < cw) return -1;
    if (shift3 < 2) shift3 = 2;

    if (frac_x == 0 && frac_y == 0) {
        for (j = 0; j < ch; j++)
            for (i = 0; i < cw; i++) {
                int sx = clip_i(int_x + (int)i, 0, ref->c_width - 1);
                int sy = clip_i(int_y + (int)j, 0, ref->c_height - 1);
                out[j * out_stride + i] =
                    (int16_t)(src[sy * ref->c_stride + sx] << shift3);
            }
    } else if (frac_y == 0 || frac_x == 0) {
        const int16_t *coeff =
            CHROMA_FILTER[frac_x ? frac_x : frac_y];
        for (j = 0; j < ch; j++)
            for (i = 0; i < cw; i++) {
                int sum = 0, k;
                for (k = 0; k < 4; k++) {
                    int sx = frac_x
                                 ? clip_i(int_x + (int)i + k - 1, 0,
                                          ref->c_width - 1)
                                 : clip_i(int_x + (int)i, 0,
                                          ref->c_width - 1);
                    int sy = frac_y
                                 ? clip_i(int_y + (int)j + k - 1, 0,
                                          ref->c_height - 1)
                                 : clip_i(int_y + (int)j, 0,
                                          ref->c_height - 1);
                    sum += src[sy * ref->c_stride + sx] * coeff[k];
                }
                out[j * out_stride + i] = (int16_t)(sum >> shift1);
            }
    } else {
        uint32_t tmp_h = ch + 3;
        if ((size_t)cw * tmp_h > scratch_n) return -1;
        for (j = 0; j < tmp_h; j++)
            for (i = 0; i < cw; i++) {
                int sum = 0, k;
                int sy = clip_i(int_y + (int)j - 1, 0,
                                ref->c_height - 1);
                for (k = 0; k < 4; k++) {
                    int sx = clip_i(int_x + (int)i + k - 1, 0,
                                    ref->c_width - 1);
                    sum += src[sy * ref->c_stride + sx] *
                           CHROMA_FILTER[frac_x][k];
                }
                scratch[j * cw + i] = sum >> shift1;
            }
        for (j = 0; j < ch; j++)
            for (i = 0; i < cw; i++) {
                int64_t sum = 0;
                int k;
                for (k = 0; k < 4; k++)
                    sum += (int64_t)scratch[(j + (uint32_t)k) * cw + i] *
                           CHROMA_FILTER[frac_y][k];
                out[j * out_stride + i] = (int16_t)(sum >> 6);
            }
    }
    return 0;
}

int heic_mc_chroma(const heic_frame *ref, heic_frame *dst, heic_mv mv,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   int32_t *scratch, size_t scratch_n)
{
    int sub_x, sub_y;
    if (!ref || !dst || ref->chroma_format == 0) return 0;
    if (!ref->cb || !ref->cr || !dst->cb || !dst->cr) return -1;
    sub_x = ref->chroma_format == 3 ? 1 : 2;
    sub_y = ref->chroma_format == 1 ? 2 : 1;
    if (mc_chroma_plane(ref, dst, ref->cb, dst->cb, mv, x, y, w, h,
                        sub_x, sub_y, scratch, scratch_n) != 0)
        return -1;
    return mc_chroma_plane(ref, dst, ref->cr, dst->cr, mv, x, y, w, h,
                           sub_x, sub_y, scratch, scratch_n);
}

int heic_mc_chroma_internal(const heic_frame *ref, heic_mv mv,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            int16_t *out_cb, int16_t *out_cr,
                            uint32_t out_stride,
                            int32_t *scratch, size_t scratch_n)
{
    int sub_x, sub_y;
    if (!ref || ref->chroma_format == 0) return 0;
    if (!ref->cb || !ref->cr || !out_cb || !out_cr) return -1;
    sub_x = ref->chroma_format == 3 ? 1 : 2;
    sub_y = ref->chroma_format == 1 ? 2 : 1;
    if (mc_chroma_plane_internal(ref, ref->cb, mv, x, y, w, h,
                                 sub_x, sub_y, out_cb, out_stride,
                                 scratch, scratch_n) != 0)
        return -1;
    return mc_chroma_plane_internal(ref, ref->cr, mv, x, y, w, h,
                                    sub_x, sub_y, out_cr, out_stride,
                                    scratch, scratch_n);
}
