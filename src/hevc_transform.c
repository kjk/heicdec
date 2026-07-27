/* hevc_transform.c -- inverse transform + dequant (port of imazen/heic transform.rs) */
#include "heic_internal.h"

/* Separable IDCT is not recursive; one TLS intermediate is enough for 8/16/32. */
#if defined(_MSC_VER)
#define HEIC_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define HEIC_TLS __thread
#else
#define HEIC_TLS
#endif

static HEIC_TLS int32_t heic_idct_scratch[1024];
static HEIC_TLS int heic_transform_clip_bits = 15;

int32_t *heic_idct_scratch_buf(void)
{
    return heic_idct_scratch;
}

static const int16_t HEIC_DST4[4][4] = {
    {29, 55, 74, 84},
    {74, 74, 0, -74},
    {84, -29, -74, 55},
    {55, -84, 74, -29},
};
static const int16_t HEIC_DCT4[4][4] = {
    {64, 64, 64, 64},
    {83, 36, -36, -83},
    {64, -64, -64, 64},
    {36, -83, 83, -36},
};

static inline int32_t htx_clip_range(int64_t v, int bits)
{
    int32_t min_v = bits >= 31 ? INT32_MIN : -(1 << bits);
    int32_t max_v = bits >= 31 ? INT32_MAX : (1 << bits) - 1;
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return (int32_t)v;
}

static inline int32_t htx_clip16(int64_t v)
{
    return htx_clip_range(v, heic_transform_clip_bits);
}

/* True if only coeff[0] (DC) may be non-zero in an n×n block (row-major). */
static int htx_only_dc(const int16_t *coeffs, int n)
{
    int i, nn = n * n;
    for (i = 1; i < nn; i++) {
        if (coeffs[i]) return 0;
    }
    return 1;
}

/* HEVC DCT pure-DC 2D IDCT: all samples equal after two 1D passes. */
static void htx_idct_dc_fill(int16_t *output, int n, int16_t dc, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t add1 = 1 << (shift1 - 1), add2 = 1 << (shift2 - 1);
    int32_t v1 = htx_clip16((64 * (int32_t)dc + add1) >> shift1);
    int16_t v = (int16_t)htx_clip16((64 * v1 + add2) >> shift2);
    int i, nn = n * n;
    for (i = 0; i < nn; i++) output[i] = v;
}

/* Column of n samples all zero? (stride = n between vertical neighbours). */
static int htx_col_zero(const int16_t *coeffs, int n, int col)
{
    int k;
    for (k = 0; k < n; k++) {
        if (coeffs[k * n + col]) return 0;
    }
    return 1;
}

void heic_idst4(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int add1 = 1 << (shift1 - 1), add2 = 1 << (shift2 - 1);
    int32_t tmp[16];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int32_t sum = 0;
            for (k = 0; k < 4; k++)
                sum += (int32_t)HEIC_DST4[k][j] * coeffs[k * 4 + i];
            tmp[j * 4 + i] = htx_clip16((sum + add1) >> shift1);
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int32_t sum = 0;
            for (k = 0; k < 4; k++)
                sum += (int32_t)HEIC_DST4[k][j] * tmp[i * 4 + k];
            output[i * 4 + j] = (int16_t)((sum + add2) >> shift2);
        }
    }
}

void heic_idct4(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int add1 = 1 << (shift1 - 1), add2 = 1 << (shift2 - 1);
    int32_t tmp[16];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int32_t sum = 0;
            for (k = 0; k < 4; k++)
                sum += (int32_t)HEIC_DCT4[k][j] * coeffs[k * 4 + i];
            tmp[j * 4 + i] = htx_clip16((sum + add1) >> shift1);
        }
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int32_t sum = 0;
            for (k = 0; k < 4; k++)
                sum += (int32_t)HEIC_DCT4[k][j] * tmp[i * 4 + k];
            output[i * 4 + j] = (int16_t)((sum + add2) >> shift2);
        }
    }
}

static void htx_idct8_1d(const int32_t src[8], int32_t dst[8], int shift)
{
    int64_t add = (int64_t)1 << (shift - 1);
    int64_t o0 = 89 * (int64_t)src[1] + 75 * src[3] + 50 * src[5] + 18 * src[7];
    int64_t o1 = 75 * (int64_t)src[1] - 18 * src[3] - 89 * src[5] - 50 * src[7];
    int64_t o2 = 50 * (int64_t)src[1] - 89 * src[3] + 18 * src[5] + 75 * src[7];
    int64_t o3 = 18 * (int64_t)src[1] - 50 * src[3] + 75 * src[5] - 89 * src[7];
    int64_t ee0 = 64 * (int64_t)src[0] + 64 * src[4];
    int64_t ee1 = 64 * (int64_t)src[0] - 64 * src[4];
    int64_t eo0 = 83 * (int64_t)src[2] + 36 * src[6];
    int64_t eo1 = 36 * (int64_t)src[2] - 83 * src[6];
    int64_t e0 = ee0 + eo0, e1 = ee1 + eo1;
    int64_t e2 = ee1 - eo1, e3 = ee0 - eo0;
    dst[0] = htx_clip16((e0 + o0 + add) >> shift);
    dst[1] = htx_clip16((e1 + o1 + add) >> shift);
    dst[2] = htx_clip16((e2 + o2 + add) >> shift);
    dst[3] = htx_clip16((e3 + o3 + add) >> shift);
    dst[4] = htx_clip16((e3 - o3 + add) >> shift);
    dst[5] = htx_clip16((e2 - o2 + add) >> shift);
    dst[6] = htx_clip16((e1 - o1 + add) >> shift);
    dst[7] = htx_clip16((e0 - o0 + add) >> shift);
}

void heic_idct8(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t tmp[64];
    int col, row;
    if (heic_simd_idct8(coeffs, output, bit_depth)) return;
    if (htx_only_dc(coeffs, 8)) {
        htx_idct_dc_fill(output, 8, coeffs[0], bit_depth);
        return;
    }
    for (col = 0; col < 8; col++) {
        int32_t src[8], d[8];
        int k;
        if (htx_col_zero(coeffs, 8, col)) {
            for (row = 0; row < 8; row++) tmp[row * 8 + col] = 0;
            continue;
        }
        for (k = 0; k < 8; k++) src[k] = coeffs[k * 8 + col];
        htx_idct8_1d(src, d, shift1);
        for (row = 0; row < 8; row++) tmp[row * 8 + col] = d[row];
    }
    for (row = 0; row < 8; row++) {
        int32_t src[8], d[8];
        int base = row * 8, col2;
        for (col2 = 0; col2 < 8; col2++) src[col2] = tmp[base + col2];
        htx_idct8_1d(src, d, shift2);
        for (col2 = 0; col2 < 8; col2++) output[base + col2] = (int16_t)d[col2];
    }
}

static void htx_idct16_1d(const int32_t src[16], int32_t dst[16], int shift)
{
    int64_t add = (int64_t)1 << (shift - 1);
    int64_t s1 = src[1], s3 = src[3], s5 = src[5], s7 = src[7];
    int64_t s9 = src[9], s11 = src[11], s13 = src[13], s15 = src[15];
    int64_t o0 = 90 * s1 + 87 * s3 + 80 * s5 + 70 * s7 + 57 * s9 + 43 * s11 + 25 * s13 + 9 * s15;
    int64_t o1 = 87 * s1 + 57 * s3 + 9 * s5 - 43 * s7 - 80 * s9 - 90 * s11 - 70 * s13 - 25 * s15;
    int64_t o2 = 80 * s1 + 9 * s3 - 70 * s5 - 87 * s7 - 25 * s9 + 57 * s11 + 90 * s13 + 43 * s15;
    int64_t o3 = 70 * s1 - 43 * s3 - 87 * s5 + 9 * s7 + 90 * s9 + 25 * s11 - 80 * s13 - 57 * s15;
    int64_t o4 = 57 * s1 - 80 * s3 - 25 * s5 + 90 * s7 - 9 * s9 - 87 * s11 + 43 * s13 + 70 * s15;
    int64_t o5 = 43 * s1 - 90 * s3 + 57 * s5 + 25 * s7 - 87 * s9 + 70 * s11 + 9 * s13 - 80 * s15;
    int64_t o6 = 25 * s1 - 70 * s3 + 90 * s5 - 80 * s7 + 43 * s9 + 9 * s11 - 57 * s13 + 87 * s15;
    int64_t o7 = 9 * s1 - 25 * s3 + 43 * s5 - 57 * s7 + 70 * s9 - 80 * s11 + 87 * s13 - 90 * s15;
    int64_t s0 = src[0], s2 = src[2], s4 = src[4], s6 = src[6];
    int64_t s8 = src[8], s10 = src[10], s12 = src[12], s14 = src[14];
    int64_t eo0 = 89 * s2 + 75 * s6 + 50 * s10 + 18 * s14;
    int64_t eo1 = 75 * s2 - 18 * s6 - 89 * s10 - 50 * s14;
    int64_t eo2 = 50 * s2 - 89 * s6 + 18 * s10 + 75 * s14;
    int64_t eo3 = 18 * s2 - 50 * s6 + 75 * s10 - 89 * s14;
    int64_t eee0 = 64 * s0 + 64 * s8, eee1 = 64 * s0 - 64 * s8;
    int64_t eeo0 = 83 * s4 + 36 * s12, eeo1 = 36 * s4 - 83 * s12;
    int64_t ee0 = eee0 + eeo0, ee1 = eee1 + eeo1;
    int64_t ee2 = eee1 - eeo1, ee3 = eee0 - eeo0;
    int64_t e0 = ee0 + eo0, e1 = ee1 + eo1, e2 = ee2 + eo2, e3 = ee3 + eo3;
    int64_t e4 = ee3 - eo3, e5 = ee2 - eo2, e6 = ee1 - eo1, e7 = ee0 - eo0;
    dst[0] = htx_clip16((e0 + o0 + add) >> shift);
    dst[1] = htx_clip16((e1 + o1 + add) >> shift);
    dst[2] = htx_clip16((e2 + o2 + add) >> shift);
    dst[3] = htx_clip16((e3 + o3 + add) >> shift);
    dst[4] = htx_clip16((e4 + o4 + add) >> shift);
    dst[5] = htx_clip16((e5 + o5 + add) >> shift);
    dst[6] = htx_clip16((e6 + o6 + add) >> shift);
    dst[7] = htx_clip16((e7 + o7 + add) >> shift);
    dst[8] = htx_clip16((e7 - o7 + add) >> shift);
    dst[9] = htx_clip16((e6 - o6 + add) >> shift);
    dst[10] = htx_clip16((e5 - o5 + add) >> shift);
    dst[11] = htx_clip16((e4 - o4 + add) >> shift);
    dst[12] = htx_clip16((e3 - o3 + add) >> shift);
    dst[13] = htx_clip16((e2 - o2 + add) >> shift);
    dst[14] = htx_clip16((e1 - o1 + add) >> shift);
    dst[15] = htx_clip16((e0 - o0 + add) >> shift);
}

void heic_idct16(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t *tmp = heic_idct_scratch_buf();
    int col, row, k, last_col;
    if (heic_simd_idct16(coeffs, output, bit_depth)) return;
    if (htx_only_dc(coeffs, 16)) {
        htx_idct_dc_fill(output, 16, coeffs[0], bit_depth);
        return;
    }
    last_col = 15;
    while (last_col > 0 && htx_col_zero(coeffs, 16, last_col)) last_col--;
    memset(tmp, 0, 256 * sizeof(int32_t));
    for (col = 0; col <= last_col; col++) {
        int32_t src[16], d[16];
        if (htx_col_zero(coeffs, 16, col)) continue;
        for (k = 0; k < 16; k++) src[k] = coeffs[k * 16 + col];
        htx_idct16_1d(src, d, shift1);
        for (row = 0; row < 16; row++) tmp[row * 16 + col] = d[row];
    }
    for (row = 0; row < 16; row++) {
        int32_t src[16], d[16];
        int base = row * 16;
        for (k = 0; k < 16; k++) src[k] = tmp[base + k];
        htx_idct16_1d(src, d, shift2);
        for (k = 0; k < 16; k++) output[base + k] = (int16_t)d[k];
    }
}

static void htx_idct32_1d(const int32_t src[32], int32_t dst[32], int shift)
{
    int64_t add = (int64_t)1 << (shift - 1);
    int64_t s1 = src[1], s3 = src[3], s5 = src[5], s7 = src[7];
    int64_t s9 = src[9], s11 = src[11], s13 = src[13], s15 = src[15];
    int64_t s17 = src[17], s19 = src[19], s21 = src[21], s23 = src[23];
    int64_t s25 = src[25], s27 = src[27], s29 = src[29], s31 = src[31];
    int64_t o0 = 90*s1+90*s3+88*s5+85*s7+82*s9+78*s11+73*s13+67*s15
               +61*s17+54*s19+46*s21+38*s23+31*s25+22*s27+13*s29+4*s31;
    int64_t o1 = 90*s1+82*s3+67*s5+46*s7+22*s9-4*s11-31*s13-54*s15
               -73*s17-85*s19-90*s21-88*s23-78*s25-61*s27-38*s29-13*s31;
    int64_t o2 = 88*s1+67*s3+31*s5-13*s7-54*s9-82*s11-90*s13-78*s15
               -46*s17-4*s19+38*s21+73*s23+90*s25+85*s27+61*s29+22*s31;
    int64_t o3 = 85*s1+46*s3-13*s5-67*s7-90*s9-73*s11-22*s13+38*s15
               +82*s17+88*s19+54*s21-4*s23-61*s25-90*s27-78*s29-31*s31;
    int64_t o4 = 82*s1+22*s3-54*s5-90*s7-61*s9+13*s11+78*s13+85*s15+31*s17
               -46*s19-90*s21-67*s23+4*s25+73*s27+88*s29+38*s31;
    int64_t o5 = 78*s1-4*s3-82*s5-73*s7+13*s9+85*s11+67*s13-22*s15
               -88*s17-61*s19+31*s21+90*s23+54*s25-38*s27-90*s29-46*s31;
    int64_t o6 = 73*s1-31*s3-90*s5-22*s7+78*s9+67*s11-38*s13-90*s15-13*s17
               +82*s19+61*s21-46*s23-88*s25-4*s27+85*s29+54*s31;
    int64_t o7 = 67*s1-54*s3-78*s5+38*s7+85*s9-22*s11-90*s13+4*s15
               +90*s17+13*s19-88*s21-31*s23+82*s25+46*s27-73*s29-61*s31;
    int64_t o8 = 61*s1-73*s3-46*s5+82*s7+31*s9-88*s11-13*s13+90*s15
               -4*s17-90*s19+22*s21+85*s23-38*s25-78*s27+54*s29+67*s31;
    int64_t o9 = 54*s1-85*s3-4*s5+88*s7-46*s9-61*s11+82*s13+13*s15
               -90*s17+38*s19+67*s21-78*s23-22*s25+90*s27-31*s29-73*s31;
    int64_t o10 = 46*s1-90*s3+38*s5+54*s7-90*s9+31*s11+61*s13-88*s15
                +22*s17+67*s19-85*s21+13*s23+73*s25-82*s27+4*s29+78*s31;
    int64_t o11 = 38*s1-88*s3+73*s5-4*s7-67*s9+90*s11-46*s13-31*s15
                +85*s17-78*s19+13*s21+61*s23-90*s25+54*s27+22*s29-82*s31;
    int64_t o12 = 31*s1-78*s3+90*s5-61*s7+4*s9+54*s11-88*s13+82*s15
                -38*s17-22*s19+73*s21-90*s23+67*s25-13*s27-46*s29+85*s31;
    int64_t o13 = 22*s1-61*s3+85*s5-90*s7+73*s9-38*s11-4*s13+46*s15
                -78*s17+90*s19-82*s21+54*s23-13*s25-31*s27+67*s29-88*s31;
    int64_t o14 = 13*s1-38*s3+61*s5-78*s7+88*s9-90*s11+85*s13-73*s15
                +54*s17-31*s19+4*s21+22*s23-46*s25+67*s27-82*s29+90*s31;
    int64_t o15 = 4*s1-13*s3+22*s5-31*s7+38*s9-46*s11+54*s13-61*s15
                +67*s17-73*s19+78*s21-82*s23+85*s25-88*s27+90*s29-90*s31;
    {
        int64_t s0 = src[0], s2 = src[2], s4 = src[4], s6 = src[6];
        int64_t s8 = src[8], s10 = src[10], s12 = src[12], s14 = src[14];
        int64_t s16 = src[16], s18 = src[18], s20 = src[20], s22 = src[22];
        int64_t s24 = src[24], s26 = src[26], s28 = src[28], s30 = src[30];
        int64_t eo0 = 90*s2+87*s6+80*s10+70*s14+57*s18+43*s22+25*s26+9*s30;
        int64_t eo1 = 87*s2+57*s6+9*s10-43*s14-80*s18-90*s22-70*s26-25*s30;
        int64_t eo2 = 80*s2+9*s6-70*s10-87*s14-25*s18+57*s22+90*s26+43*s30;
        int64_t eo3 = 70*s2-43*s6-87*s10+9*s14+90*s18+25*s22-80*s26-57*s30;
        int64_t eo4 = 57*s2-80*s6-25*s10+90*s14-9*s18-87*s22+43*s26+70*s30;
        int64_t eo5 = 43*s2-90*s6+57*s10+25*s14-87*s18+70*s22+9*s26-80*s30;
        int64_t eo6 = 25*s2-70*s6+90*s10-80*s14+43*s18+9*s22-57*s26+87*s30;
        int64_t eo7 = 9*s2-25*s6+43*s10-57*s14+70*s18-80*s22+87*s26-90*s30;
        int64_t eeo0 = 89*s4+75*s12+50*s20+18*s28;
        int64_t eeo1 = 75*s4-18*s12-89*s20-50*s28;
        int64_t eeo2 = 50*s4-89*s12+18*s20+75*s28;
        int64_t eeo3 = 18*s4-50*s12+75*s20-89*s28;
        int64_t eeee0 = 64*s0+64*s16, eeee1 = 64*s0-64*s16;
        int64_t eeeo0 = 83*s8+36*s24, eeeo1 = 36*s8-83*s24;
        int64_t eee0 = eeee0+eeeo0, eee1 = eeee1+eeeo1;
        int64_t eee2 = eeee1-eeeo1, eee3 = eeee0-eeeo0;
        int64_t ee0 = eee0+eeo0, ee1 = eee1+eeo1, ee2 = eee2+eeo2, ee3 = eee3+eeo3;
        int64_t ee4 = eee3-eeo3, ee5 = eee2-eeo2, ee6 = eee1-eeo1, ee7 = eee0-eeo0;
        int64_t e0 = ee0+eo0, e1 = ee1+eo1, e2 = ee2+eo2, e3 = ee3+eo3;
        int64_t e4 = ee4+eo4, e5 = ee5+eo5, e6 = ee6+eo6, e7 = ee7+eo7;
        int64_t e8 = ee7-eo7, e9 = ee6-eo6, e10 = ee5-eo5, e11 = ee4-eo4;
        int64_t e12 = ee3-eo3, e13 = ee2-eo2, e14 = ee1-eo1, e15 = ee0-eo0;
        dst[0] = htx_clip16((e0 + o0 + add) >> shift);
        dst[1] = htx_clip16((e1 + o1 + add) >> shift);
        dst[2] = htx_clip16((e2 + o2 + add) >> shift);
        dst[3] = htx_clip16((e3 + o3 + add) >> shift);
        dst[4] = htx_clip16((e4 + o4 + add) >> shift);
        dst[5] = htx_clip16((e5 + o5 + add) >> shift);
        dst[6] = htx_clip16((e6 + o6 + add) >> shift);
        dst[7] = htx_clip16((e7 + o7 + add) >> shift);
        dst[8] = htx_clip16((e8 + o8 + add) >> shift);
        dst[9] = htx_clip16((e9 + o9 + add) >> shift);
        dst[10] = htx_clip16((e10 + o10 + add) >> shift);
        dst[11] = htx_clip16((e11 + o11 + add) >> shift);
        dst[12] = htx_clip16((e12 + o12 + add) >> shift);
        dst[13] = htx_clip16((e13 + o13 + add) >> shift);
        dst[14] = htx_clip16((e14 + o14 + add) >> shift);
        dst[15] = htx_clip16((e15 + o15 + add) >> shift);
        dst[16] = htx_clip16((e15 - o15 + add) >> shift);
        dst[17] = htx_clip16((e14 - o14 + add) >> shift);
        dst[18] = htx_clip16((e13 - o13 + add) >> shift);
        dst[19] = htx_clip16((e12 - o12 + add) >> shift);
        dst[20] = htx_clip16((e11 - o11 + add) >> shift);
        dst[21] = htx_clip16((e10 - o10 + add) >> shift);
        dst[22] = htx_clip16((e9 - o9 + add) >> shift);
        dst[23] = htx_clip16((e8 - o8 + add) >> shift);
        dst[24] = htx_clip16((e7 - o7 + add) >> shift);
        dst[25] = htx_clip16((e6 - o6 + add) >> shift);
        dst[26] = htx_clip16((e5 - o5 + add) >> shift);
        dst[27] = htx_clip16((e4 - o4 + add) >> shift);
        dst[28] = htx_clip16((e3 - o3 + add) >> shift);
        dst[29] = htx_clip16((e2 - o2 + add) >> shift);
        dst[30] = htx_clip16((e1 - o1 + add) >> shift);
        dst[31] = htx_clip16((e0 - o0 + add) >> shift);
    }
}

void heic_idct32(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t *tmp = heic_idct_scratch_buf();
    int col, row, k, last_col;
    if (heic_simd_idct32(coeffs, output, bit_depth)) return;
    if (htx_only_dc(coeffs, 32)) {
        htx_idct_dc_fill(output, 32, coeffs[0], bit_depth);
        return;
    }
    /* Rightmost non-zero column; skip IDCT on trailing zero columns. */
    last_col = 31;
    while (last_col > 0 && htx_col_zero(coeffs, 32, last_col)) last_col--;
    memset(tmp, 0, 1024 * sizeof(int32_t));
    for (col = 0; col <= last_col; col++) {
        int32_t src[32], d[32];
        if (htx_col_zero(coeffs, 32, col)) continue;
        for (k = 0; k < 32; k++) src[k] = coeffs[k * 32 + col];
        htx_idct32_1d(src, d, shift1);
        for (row = 0; row < 32; row++) tmp[row * 32 + col] = d[row];
    }
    for (row = 0; row < 32; row++) {
        int32_t src[32], d[32];
        int base = row * 32;
        int allz = 1;
        for (k = 0; k <= last_col; k++) {
            if (tmp[base + k]) {
                allz = 0;
                break;
            }
        }
        if (allz) {
            memset(output + base, 0, 32 * sizeof(int16_t));
            continue;
        }
        for (k = 0; k < 32; k++) src[k] = tmp[base + k];
        htx_idct32_1d(src, d, shift2);
        for (k = 0; k < 32; k++) output[base + k] = (int16_t)d[k];
    }
}

void heic_dequantize(int16_t *coeffs, int n, int qp, int bit_depth,
                     uint8_t log2_tr_size)
{
    static const int32_t LEVEL_SCALE[6] = {40, 45, 51, 57, 64, 72};
    int32_t qp_clamped = qp > 180 ? 180 : qp;
    int32_t qp_per = qp_clamped / 6;
    int32_t qp_rem = qp_clamped % 6;
    int64_t combined = ((int64_t)LEVEL_SCALE[qp_rem]) << qp_per;
    int32_t shift = (int32_t)bit_depth - 9 + (int32_t)log2_tr_size;
    int i;
    if (shift >= 0) {
        int64_t add = shift > 0 ? (1LL << (shift - 1)) : 0;
        /* |coeff|≤32768, combined≤65536 → product fits int32. */
        if (combined <= 65536 && shift < 31) {
            int32_t c32 = (int32_t)combined;
            int32_t add32 = (int32_t)add;
            if (heic_simd_dequant(coeffs, n, c32, shift))
                return;
            for (i = 0; i < n; i++) {
                int32_t c = coeffs[i];
                int32_t v;
                if (c == 0) continue;
                v = (c * c32 + add32) >> shift;
                if (v < -32768) v = -32768;
                if (v > 32767) v = 32767;
                coeffs[i] = (int16_t)v;
            }
        } else {
            for (i = 0; i < n; i++) {
                int32_t c = coeffs[i];
                int64_t v;
                if (c == 0) continue;
                v = ((int64_t)c * combined + add) >> shift;
                if (v < -32768) v = -32768;
                if (v > 32767) v = 32767;
                coeffs[i] = (int16_t)v;
            }
        }
    } else {
        int neg = -shift;
        for (i = 0; i < n; i++) {
            int32_t c = coeffs[i];
            int64_t v;
            if (c == 0) continue;
            v = ((int64_t)c * combined) << neg;
            if (v < -32768) v = -32768;
            if (v > 32767) v = 32767;
            coeffs[i] = (int16_t)v;
        }
    }
}

void heic_dequantize_scaled(int16_t *coeffs, int n, int qp, int bit_depth,
                            uint8_t log2_tr_size, const heic_scaling_list *list,
                            uint8_t matrix_id)
{
    static const int32_t LEVEL_SCALE[6] = {40, 45, 51, 57, 64, 72};
    static const uint8_t DIAG_INV_4X4[16] = {
        0, 1, 3, 6, 2, 4, 7, 10, 5, 8, 11, 13, 9, 12, 14, 15
    };
    static const uint8_t DIAG_INV_8X8[64] = {
         0,  1,  3,  6, 10, 15, 21, 28,
         2,  4,  7, 11, 16, 22, 29, 36,
         5,  8, 12, 17, 23, 30, 37, 43,
         9, 13, 18, 24, 31, 38, 44, 49,
        14, 19, 25, 32, 39, 45, 50, 54,
        20, 26, 33, 40, 46, 51, 55, 58,
        27, 34, 41, 47, 52, 56, 59, 61,
        35, 42, 48, 53, 57, 60, 62, 63
    };
    int size = 1 << log2_tr_size;
    int size_id = (int)log2_tr_size - 2;
    int qp_clamped = qp > 180 ? 180 : qp;
    int qp_per = qp_clamped / 6;
    int qp_rem = qp_clamped % 6;
    int shift = bit_depth + (int)log2_tr_size - 5;
    int64_t qp_scale = 1LL << qp_per;
    int64_t add = shift > 0 ? 1LL << (shift - 1) : 0;
    int i;

    if (!list || size_id < 0 || size_id > 3 || matrix_id > 5) {
        heic_dequantize(coeffs, n, qp, bit_depth, log2_tr_size);
        return;
    }
    /* Raster walk with zero-skip: most residual positions are zero after
     * CABAC, and scale lookup + 64-bit multiply dominate the old full loop. */
    {
        int x = 0, y = 0;
        int level = LEVEL_SCALE[qp_rem];
        for (i = 0; i < n; i++) {
            uint8_t scale;
            int64_t value;
            int c = coeffs[i];
            if (c == 0) {
                if (++x == size) {
                    x = 0;
                    y++;
                }
                continue;
            }
            if (size_id == 0) {
                scale = list->coef[0][matrix_id][DIAG_INV_4X4[i]];
            } else if (size_id == 1) {
                scale = list->coef[1][matrix_id][DIAG_INV_8X8[i]];
            } else {
                int divisor = size_id == 2 ? 2 : 4;
                int sx = x / divisor, sy = y / divisor;
                if (x == 0 && y == 0)
                    scale = list->dc_coef[size_id - 2][matrix_id];
                else
                    scale = list->coef[size_id][matrix_id]
                                      [DIAG_INV_8X8[sy * 8 + sx]];
            }
            value = (int64_t)c * scale * level * qp_scale;
            if (shift >= 0)
                value = (value + add) >> shift;
            else
                value <<= -shift;
            if (value < -32768) value = -32768;
            if (value > 32767) value = 32767;
            coeffs[i] = (int16_t)value;
            if (++x == size) {
                x = 0;
                y++;
            }
        }
    }
}

void heic_dequantize_extended(int32_t *coeffs, int n, int qp, int bit_depth,
                              uint8_t log2_tr_size, int max_transform_range)
{
    static const int32_t LEVEL_SCALE[6] = {40, 45, 51, 57, 64, 72};
    int32_t qp_clamped = qp > 180 ? 180 : qp;
    int32_t qp_per = qp_clamped / 6;
    int32_t qp_rem = qp_clamped % 6;
    int64_t combined = ((int64_t)LEVEL_SCALE[qp_rem]) << qp_per;
    int32_t shift = bit_depth + (int32_t)log2_tr_size + 6
                    - max_transform_range;
    int i;
    if (shift >= 0) {
        int64_t add = shift > 0 ? (1LL << (shift - 1)) : 0;
        for (i = 0; i < n; i++) {
            int64_t value = ((int64_t)coeffs[i] * combined + add) >> shift;
            coeffs[i] = htx_clip_range(value, max_transform_range);
        }
    } else {
        for (i = 0; i < n; i++) {
            int64_t value = ((int64_t)coeffs[i] * combined) << -shift;
            coeffs[i] = htx_clip_range(value, max_transform_range);
        }
    }
}

void heic_dequantize_scaled_extended(
    int32_t *coeffs, int n, int qp, int bit_depth, uint8_t log2_tr_size,
    int max_transform_range, const heic_scaling_list *list, uint8_t matrix_id)
{
    static const int32_t LEVEL_SCALE[6] = {40, 45, 51, 57, 64, 72};
    static const uint8_t DIAG_INV_4X4[16] = {
        0, 1, 3, 6, 2, 4, 7, 10, 5, 8, 11, 13, 9, 12, 14, 15
    };
    static const uint8_t DIAG_INV_8X8[64] = {
         0,  1,  3,  6, 10, 15, 21, 28,
         2,  4,  7, 11, 16, 22, 29, 36,
         5,  8, 12, 17, 23, 30, 37, 43,
         9, 13, 18, 24, 31, 38, 44, 49,
        14, 19, 25, 32, 39, 45, 50, 54,
        20, 26, 33, 40, 46, 51, 55, 58,
        27, 34, 41, 47, 52, 56, 59, 61,
        35, 42, 48, 53, 57, 60, 62, 63
    };
    int size = 1 << log2_tr_size;
    int size_id = (int)log2_tr_size - 2;
    int qp_clamped = qp > 180 ? 180 : qp;
    int qp_per = qp_clamped / 6;
    int qp_rem = qp_clamped % 6;
    int shift = bit_depth + (int)log2_tr_size + 10 - max_transform_range;
    int64_t qp_scale = 1LL << qp_per;
    int64_t add = shift > 0 ? 1LL << (shift - 1) : 0;
    int i;

    if (!list || size_id < 0 || size_id > 3 || matrix_id > 5) {
        heic_dequantize_extended(coeffs, n, qp, bit_depth, log2_tr_size,
                                 max_transform_range);
        return;
    }
    for (i = 0; i < n; i++) {
        int x = i % size, y = i / size;
        uint8_t scale;
        int64_t value;
        if (size_id == 0) {
            scale = list->coef[0][matrix_id][DIAG_INV_4X4[i]];
        } else if (size_id == 1) {
            scale = list->coef[1][matrix_id][DIAG_INV_8X8[i]];
        } else {
            int divisor = size_id == 2 ? 2 : 4;
            int sx = x / divisor, sy = y / divisor;
            if (x == 0 && y == 0)
                scale = list->dc_coef[size_id - 2][matrix_id];
            else
                scale = list->coef[size_id][matrix_id]
                                  [DIAG_INV_8X8[sy * 8 + sx]];
        }
        value = (int64_t)coeffs[i] * scale * LEVEL_SCALE[qp_rem] * qp_scale;
        if (shift >= 0)
            value = (value + add) >> shift;
        else
            value <<= -shift;
        coeffs[i] = htx_clip_range(value, max_transform_range);
    }
}

static int htx_col_zero32(const int32_t *coeffs, int n, int col)
{
    int k;
    for (k = 0; k < n; k++) {
        if (coeffs[k * n + col]) return 0;
    }
    return 1;
}

static void htx_transform4_extended(
    const int32_t *coeffs, int32_t *output, const int16_t matrix[4][4],
    int shift1, int shift2, int max_transform_range)
{
    int32_t tmp[16];
    int i, j, k;
    heic_transform_clip_bits = max_transform_range;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int64_t sum = 0;
            for (k = 0; k < 4; k++)
                sum += (int64_t)matrix[k][j] * coeffs[k * 4 + i];
            tmp[j * 4 + i] = htx_clip16(
                (sum + ((int64_t)1 << (shift1 - 1))) >> shift1);
        }
    }
    heic_transform_clip_bits = 31;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int64_t sum = 0;
            for (k = 0; k < 4; k++)
                sum += (int64_t)matrix[k][j] * tmp[i * 4 + k];
            output[i * 4 + j] = htx_clip16(
                (sum + ((int64_t)1 << (shift2 - 1))) >> shift2);
        }
    }
}

void heic_inverse_transform_extended(
    const int32_t *coeffs, int32_t *output, int size, int bit_depth,
    int max_transform_range, int is_intra_4x4_luma)
{
    int shift1 = 7;
    int shift2 = max_transform_range + 5 - bit_depth;
    int32_t *tmp = heic_idct_scratch_buf();
    int col, row, k;
    if (size == 4) {
        htx_transform4_extended(
            coeffs, output,
            is_intra_4x4_luma ? HEIC_DST4 : HEIC_DCT4,
            shift1, shift2, max_transform_range);
        heic_transform_clip_bits = 15;
        return;
    }
    memset(tmp, 0, (size_t)size * size * sizeof(int32_t));
    heic_transform_clip_bits = max_transform_range;
    for (col = 0; col < size; col++) {
        int32_t src[32], dst[32];
        if (htx_col_zero32(coeffs, size, col)) continue;
        for (k = 0; k < size; k++) src[k] = coeffs[k * size + col];
        if (size == 8)
            htx_idct8_1d(src, dst, shift1);
        else if (size == 16)
            htx_idct16_1d(src, dst, shift1);
        else if (size == 32)
            htx_idct32_1d(src, dst, shift1);
        else
            break;
        for (row = 0; row < size; row++) tmp[row * size + col] = dst[row];
    }
    heic_transform_clip_bits = 31;
    for (row = 0; row < size; row++) {
        int32_t src[32], dst[32];
        int base = row * size;
        for (k = 0; k < size; k++) src[k] = tmp[base + k];
        if (size == 8)
            htx_idct8_1d(src, dst, shift2);
        else if (size == 16)
            htx_idct16_1d(src, dst, shift2);
        else if (size == 32)
            htx_idct32_1d(src, dst, shift2);
        else
            break;
        for (k = 0; k < size; k++) output[base + k] = dst[k];
    }
    heic_transform_clip_bits = 15;
}

void heic_inverse_transform(const int16_t *coeffs, int16_t *output, int size,
                            int bit_depth, int is_intra_4x4_luma)
{
    heic_inverse_transform_nnz(coeffs, output, size, bit_depth,
                               is_intra_4x4_luma, -1);
}

/* Like heic_inverse_transform, but num_nonzero from residual CABAC skips the
 * full-block zero scan used by pure-DC detection (big for 16/32 transforms).
 * Pass num_nonzero < 0 when the count is unknown. */
void heic_inverse_transform_nnz(const int16_t *coeffs, int16_t *output, int size,
                                int bit_depth, int is_intra_4x4_luma,
                                int num_nonzero)
{
    /* DST-4x4 intra is not the DCT pure-DC formula. */
    if (num_nonzero == 0) {
        memset(output, 0, (size_t)size * (size_t)size * sizeof(int16_t));
        return;
    }
    if (num_nonzero == 1 && coeffs[0] != 0
        && !(size == 4 && is_intra_4x4_luma)) {
        htx_idct_dc_fill(output, size, coeffs[0], bit_depth);
        return;
    }
    switch (size) {
    case 4:
        if (is_intra_4x4_luma) heic_idst4(coeffs, output, bit_depth);
        else heic_idct4(coeffs, output, bit_depth);
        break;
    case 8:
        heic_idct8(coeffs, output, bit_depth);
        break;
    case 16:
        heic_idct16(coeffs, output, bit_depth);
        break;
    case 32:
        heic_idct32(coeffs, output, bit_depth);
        break;
    default:
        break;
    }
}

void heic_add_residual(uint16_t *plane, int stride, int x0, int y0,
                       const int16_t *residual, int size, int max_val)
{
    int py, px;
    if (heic_simd_add_residual(plane, stride, x0, y0, residual, size, max_val))
        return;
    for (py = 0; py < size; py++) {
        uint16_t *dst = plane + (y0 + py) * stride + x0;
        const int16_t *src = residual + py * size;
        for (px = 0; px < size; px++) {
            int32_t v = (int32_t)dst[px] + (int32_t)src[px];
            if (v < 0) v = 0;
            else if (v > max_val) v = max_val;
            dst[px] = (uint16_t)v;
        }
    }
}
