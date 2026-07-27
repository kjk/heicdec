/* hevc_simd.c -- x86 SSE4.1 acceleration for IDCT, residual, color, deblock.
 *
 * - IDCT: 4-wide parallel 1D butterflies (lanes = columns/rows)
 * - Residual add: 8× epi16→epi32
 * - Color: direct vector math / LUT gather + SIMD clamp + SSSE3 RGB pack
 * - Chroma deblock: 4 samples along an edge in parallel
 */
#include "heic_internal.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define HEIC_X86 1
#include <emmintrin.h> /* SSE2 */
#include <tmmintrin.h> /* SSSE3: shuffle_epi8 */
#include <smmintrin.h> /* SSE4.1: mullo/min/max_epi32, packus_epi32 */
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#define HEIC_X86 0
#endif

static int g_simd; /* 0 = off, 1 = SSE4.1 */

void heic_simd_init(void)
{
    g_simd = 0;
#if HEIC_X86
    {
#if defined(_MSC_VER)
        int info[4] = {0, 0, 0, 0};
        __cpuid(info, 1);
        if (info[2] & (1 << 19)) g_simd = 1; /* SSE4.1 */
#elif defined(__GNUC__) || defined(__clang__)
        if (__builtin_cpu_supports("sse4.1")) g_simd = 1;
#else
        g_simd = 1; /* assume SSE4.1 on unknown x86 toolchain */
#endif
    }
#endif
}

int heic_simd_enabled(void) { return g_simd; }

#if HEIC_X86

static inline __m128i hs_mul(__m128i a, int32_t c)
{
    return _mm_mullo_epi32(a, _mm_set1_epi32(c));
}
static inline __m128i hs_add(__m128i a, __m128i b) { return _mm_add_epi32(a, b); }
static inline __m128i hs_sub(__m128i a, __m128i b) { return _mm_sub_epi32(a, b); }

static inline __m128i hs_clip16_shift(__m128i v, __m128i add, int shift)
{
    __m128i r = _mm_srai_epi32(_mm_add_epi32(v, add), shift);
    r = _mm_min_epi32(r, _mm_set1_epi32(32767));
    r = _mm_max_epi32(r, _mm_set1_epi32(-32768));
    return r;
}

/* ---- IDCT8: 4 columns × 2 batches ---- */

static void idct8_1d_x4(const __m128i s[8], __m128i d[8], int shift)
{
    __m128i add = _mm_set1_epi32(1 << (shift - 1));
    __m128i o0 = hs_add(hs_add(hs_mul(s[1], 89), hs_mul(s[3], 75)),
                        hs_add(hs_mul(s[5], 50), hs_mul(s[7], 18)));
    __m128i o1 = hs_add(hs_add(hs_mul(s[1], 75), hs_mul(s[3], -18)),
                        hs_add(hs_mul(s[5], -89), hs_mul(s[7], -50)));
    __m128i o2 = hs_add(hs_add(hs_mul(s[1], 50), hs_mul(s[3], -89)),
                        hs_add(hs_mul(s[5], 18), hs_mul(s[7], 75)));
    __m128i o3 = hs_add(hs_add(hs_mul(s[1], 18), hs_mul(s[3], -50)),
                        hs_add(hs_mul(s[5], 75), hs_mul(s[7], -89)));
    __m128i ee0 = hs_add(hs_mul(s[0], 64), hs_mul(s[4], 64));
    __m128i ee1 = hs_sub(hs_mul(s[0], 64), hs_mul(s[4], 64));
    __m128i eo0 = hs_add(hs_mul(s[2], 83), hs_mul(s[6], 36));
    __m128i eo1 = hs_sub(hs_mul(s[2], 36), hs_mul(s[6], 83));
    __m128i e0 = hs_add(ee0, eo0), e1 = hs_add(ee1, eo1);
    __m128i e2 = hs_sub(ee1, eo1), e3 = hs_sub(ee0, eo0);
    d[0] = hs_clip16_shift(hs_add(e0, o0), add, shift);
    d[1] = hs_clip16_shift(hs_add(e1, o1), add, shift);
    d[2] = hs_clip16_shift(hs_add(e2, o2), add, shift);
    d[3] = hs_clip16_shift(hs_add(e3, o3), add, shift);
    d[4] = hs_clip16_shift(hs_sub(e3, o3), add, shift);
    d[5] = hs_clip16_shift(hs_sub(e2, o2), add, shift);
    d[6] = hs_clip16_shift(hs_sub(e1, o1), add, shift);
    d[7] = hs_clip16_shift(hs_sub(e0, o0), add, shift);
}

static void load4_cols_i16(const int16_t *c, int n, int col, __m128i *s, int nfreq)
{
    int k;
    for (k = 0; k < nfreq; k++) {
        const int16_t *p = c + k * n + col;
        s[k] = _mm_setr_epi32(p[0], p[1], p[2], p[3]);
    }
}

static void store4_cols_i32(int32_t *tmp, int n, int col, const __m128i *d, int nfreq)
{
    int k;
    for (k = 0; k < nfreq; k++) {
        int32_t *p = tmp + k * n + col;
        _mm_storeu_si128((__m128i *)p, d[k]);
    }
}

static void load4_rows_i32(const int32_t *tmp, int n, int row, __m128i *s, int nfreq)
{
    int k;
    for (k = 0; k < nfreq; k++) {
        s[k] = _mm_setr_epi32(tmp[row * n + k], tmp[(row + 1) * n + k],
                              tmp[(row + 2) * n + k], tmp[(row + 3) * n + k]);
    }
}

static void store4_rows_i16(int16_t *out, int n, int row, const __m128i *d, int nfreq)
{
    int k, r;
    int32_t lane[4];
    for (k = 0; k < nfreq; k++) {
        _mm_storeu_si128((__m128i *)lane, d[k]);
        for (r = 0; r < 4; r++)
            out[(row + r) * n + k] = (int16_t)lane[r];
    }
}

/* ---- IDCT16 4-wide ---- */

static void idct16_1d_x4(const __m128i s[16], __m128i d[16], int shift)
{
    __m128i add = _mm_set1_epi32(1 << (shift - 1));
    __m128i s1 = s[1], s3 = s[3], s5 = s[5], s7 = s[7];
    __m128i s9 = s[9], s11 = s[11], s13 = s[13], s15 = s[15];
    __m128i o0, o1, o2, o3, o4, o5, o6, o7;
    __m128i eo0, eo1, eo2, eo3, eee0, eee1, eeo0, eeo1;
    __m128i ee0, ee1, ee2, ee3, e0, e1, e2, e3, e4, e5, e6, e7;

#define M4(a, c0, b, c1, c, c2, d, c3)                                         \
    hs_add(hs_add(hs_mul((a), (c0)), hs_mul((b), (c1))),                     \
           hs_add(hs_mul((c), (c2)), hs_mul((d), (c3))))

    o0 = hs_add(M4(s1, 90, s3, 87, s5, 80, s7, 70), M4(s9, 57, s11, 43, s13, 25, s15, 9));
    o1 = hs_add(M4(s1, 87, s3, 57, s5, 9, s7, -43), M4(s9, -80, s11, -90, s13, -70, s15, -25));
    o2 = hs_add(M4(s1, 80, s3, 9, s5, -70, s7, -87), M4(s9, -25, s11, 57, s13, 90, s15, 43));
    o3 = hs_add(M4(s1, 70, s3, -43, s5, -87, s7, 9), M4(s9, 90, s11, 25, s13, -80, s15, -57));
    o4 = hs_add(M4(s1, 57, s3, -80, s5, -25, s7, 90), M4(s9, -9, s11, -87, s13, 43, s15, 70));
    o5 = hs_add(M4(s1, 43, s3, -90, s5, 57, s7, 25), M4(s9, -87, s11, 70, s13, 9, s15, -80));
    o6 = hs_add(M4(s1, 25, s3, -70, s5, 90, s7, -80), M4(s9, 43, s11, 9, s13, -57, s15, 87));
    o7 = hs_add(M4(s1, 9, s3, -25, s5, 43, s7, -57), M4(s9, 70, s11, -80, s13, 87, s15, -90));

    eo0 = M4(s[2], 89, s[6], 75, s[10], 50, s[14], 18);
    eo1 = M4(s[2], 75, s[6], -18, s[10], -89, s[14], -50);
    eo2 = M4(s[2], 50, s[6], -89, s[10], 18, s[14], 75);
    eo3 = M4(s[2], 18, s[6], -50, s[10], 75, s[14], -89);
    eee0 = hs_add(hs_mul(s[0], 64), hs_mul(s[8], 64));
    eee1 = hs_sub(hs_mul(s[0], 64), hs_mul(s[8], 64));
    eeo0 = hs_add(hs_mul(s[4], 83), hs_mul(s[12], 36));
    eeo1 = hs_sub(hs_mul(s[4], 36), hs_mul(s[12], 83));
    ee0 = hs_add(eee0, eeo0);
    ee1 = hs_add(eee1, eeo1);
    ee2 = hs_sub(eee1, eeo1);
    ee3 = hs_sub(eee0, eeo0);
    e0 = hs_add(ee0, eo0);
    e1 = hs_add(ee1, eo1);
    e2 = hs_add(ee2, eo2);
    e3 = hs_add(ee3, eo3);
    e4 = hs_sub(ee3, eo3);
    e5 = hs_sub(ee2, eo2);
    e6 = hs_sub(ee1, eo1);
    e7 = hs_sub(ee0, eo0);
    d[0] = hs_clip16_shift(hs_add(e0, o0), add, shift);
    d[1] = hs_clip16_shift(hs_add(e1, o1), add, shift);
    d[2] = hs_clip16_shift(hs_add(e2, o2), add, shift);
    d[3] = hs_clip16_shift(hs_add(e3, o3), add, shift);
    d[4] = hs_clip16_shift(hs_add(e4, o4), add, shift);
    d[5] = hs_clip16_shift(hs_add(e5, o5), add, shift);
    d[6] = hs_clip16_shift(hs_add(e6, o6), add, shift);
    d[7] = hs_clip16_shift(hs_add(e7, o7), add, shift);
    d[8] = hs_clip16_shift(hs_sub(e7, o7), add, shift);
    d[9] = hs_clip16_shift(hs_sub(e6, o6), add, shift);
    d[10] = hs_clip16_shift(hs_sub(e5, o5), add, shift);
    d[11] = hs_clip16_shift(hs_sub(e4, o4), add, shift);
    d[12] = hs_clip16_shift(hs_sub(e3, o3), add, shift);
    d[13] = hs_clip16_shift(hs_sub(e2, o2), add, shift);
    d[14] = hs_clip16_shift(hs_sub(e1, o1), add, shift);
    d[15] = hs_clip16_shift(hs_sub(e0, o0), add, shift);
#undef M4
}

/* ---- IDCT32 4-wide (hot path for 4:4:4 / large stills) ---- */

static void idct32_1d_x4(const __m128i s[32], __m128i d[32], int shift)
{
    __m128i add = _mm_set1_epi32(1 << (shift - 1));
    __m128i s1 = s[1], s3 = s[3], s5 = s[5], s7 = s[7];
    __m128i s9 = s[9], s11 = s[11], s13 = s[13], s15 = s[15];
    __m128i s17 = s[17], s19 = s[19], s21 = s[21], s23 = s[23];
    __m128i s25 = s[25], s27 = s[27], s29 = s[29], s31 = s[31];
    __m128i o0, o1, o2, o3, o4, o5, o6, o7, o8, o9, o10, o11, o12, o13, o14, o15;
    __m128i eo0, eo1, eo2, eo3, eo4, eo5, eo6, eo7;
    __m128i eeo0, eeo1, eeo2, eeo3, eeee0, eeee1, eeeo0, eeeo1;
    __m128i eee0, eee1, eee2, eee3, ee0, ee1, ee2, ee3, ee4, ee5, ee6, ee7;
    __m128i e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15;

#define A2(a, ca, b, cb) hs_add(hs_mul((a), (ca)), hs_mul((b), (cb)))
#define A4(a, ca, b, cb, c, cc, d, cd)                                         \
    hs_add(A2((a), (ca), (b), (cb)), A2((c), (cc), (d), (cd)))
#define A8(a, ca, b, cb, c, cc, d, cd, e, ce, f, cf, g, cg, h, ch)             \
    hs_add(A4((a), (ca), (b), (cb), (c), (cc), (d), (cd)),                     \
           A4((e), (ce), (f), (cf), (g), (cg), (h), (ch)))

    o0 = A8(s1, 90, s3, 90, s5, 88, s7, 85, s9, 82, s11, 78, s13, 73, s15, 67);
    o0 = hs_add(o0, A8(s17, 61, s19, 54, s21, 46, s23, 38, s25, 31, s27, 22, s29, 13, s31, 4));
    o1 = A8(s1, 90, s3, 82, s5, 67, s7, 46, s9, 22, s11, -4, s13, -31, s15, -54);
    o1 = hs_add(o1, A8(s17, -73, s19, -85, s21, -90, s23, -88, s25, -78, s27, -61, s29, -38, s31, -13));
    o2 = A8(s1, 88, s3, 67, s5, 31, s7, -13, s9, -54, s11, -82, s13, -90, s15, -78);
    o2 = hs_add(o2, A8(s17, -46, s19, -4, s21, 38, s23, 73, s25, 90, s27, 85, s29, 61, s31, 22));
    o3 = A8(s1, 85, s3, 46, s5, -13, s7, -67, s9, -90, s11, -73, s13, -22, s15, 38);
    o3 = hs_add(o3, A8(s17, 82, s19, 88, s21, 54, s23, -4, s25, -61, s27, -90, s29, -78, s31, -31));
    o4 = A8(s1, 82, s3, 22, s5, -54, s7, -90, s9, -61, s11, 13, s13, 78, s15, 85);
    o4 = hs_add(o4, A8(s17, 31, s19, -46, s21, -90, s23, -67, s25, 4, s27, 73, s29, 88, s31, 38));
    o5 = A8(s1, 78, s3, -4, s5, -82, s7, -73, s9, 13, s11, 85, s13, 67, s15, -22);
    o5 = hs_add(o5, A8(s17, -88, s19, -61, s21, 31, s23, 90, s25, 54, s27, -38, s29, -90, s31, -46));
    o6 = A8(s1, 73, s3, -31, s5, -90, s7, -22, s9, 78, s11, 67, s13, -38, s15, -90);
    o6 = hs_add(o6, A8(s17, -13, s19, 82, s21, 61, s23, -46, s25, -88, s27, -4, s29, 85, s31, 54));
    o7 = A8(s1, 67, s3, -54, s5, -78, s7, 38, s9, 85, s11, -22, s13, -90, s15, 4);
    o7 = hs_add(o7, A8(s17, 90, s19, 13, s21, -88, s23, -31, s25, 82, s27, 46, s29, -73, s31, -61));
    o8 = A8(s1, 61, s3, -73, s5, -46, s7, 82, s9, 31, s11, -88, s13, -13, s15, 90);
    o8 = hs_add(o8, A8(s17, -4, s19, -90, s21, 22, s23, 85, s25, -38, s27, -78, s29, 54, s31, 67));
    o9 = A8(s1, 54, s3, -85, s5, -4, s7, 88, s9, -46, s11, -61, s13, 82, s15, 13);
    o9 = hs_add(o9, A8(s17, -90, s19, 38, s21, 67, s23, -78, s25, -22, s27, 90, s29, -31, s31, -73));
    o10 = A8(s1, 46, s3, -90, s5, 38, s7, 54, s9, -90, s11, 31, s13, 61, s15, -88);
    o10 = hs_add(o10, A8(s17, 22, s19, 67, s21, -85, s23, 13, s25, 73, s27, -82, s29, 4, s31, 78));
    o11 = A8(s1, 38, s3, -88, s5, 73, s7, -4, s9, -67, s11, 90, s13, -46, s15, -31);
    o11 = hs_add(o11, A8(s17, 85, s19, -78, s21, 13, s23, 61, s25, -90, s27, 54, s29, 22, s31, -82));
    o12 = A8(s1, 31, s3, -78, s5, 90, s7, -61, s9, 4, s11, 54, s13, -88, s15, 82);
    o12 = hs_add(o12, A8(s17, -38, s19, -22, s21, 73, s23, -90, s25, 67, s27, -13, s29, -46, s31, 85));
    o13 = A8(s1, 22, s3, -61, s5, 85, s7, -90, s9, 73, s11, -38, s13, -4, s15, 46);
    o13 = hs_add(o13, A8(s17, -78, s19, 90, s21, -82, s23, 54, s25, -13, s27, -31, s29, 67, s31, -88));
    o14 = A8(s1, 13, s3, -38, s5, 61, s7, -78, s9, 88, s11, -90, s13, 85, s15, -73);
    o14 = hs_add(o14, A8(s17, 54, s19, -31, s21, 4, s23, 22, s25, -46, s27, 67, s29, -82, s31, 90));
    o15 = A8(s1, 4, s3, -13, s5, 22, s7, -31, s9, 38, s11, -46, s13, 54, s15, -61);
    o15 = hs_add(o15, A8(s17, 67, s19, -73, s21, 78, s23, -82, s25, 85, s27, -88, s29, 90, s31, -90));

    {
        __m128i s0 = s[0], s2 = s[2], s4 = s[4], s6 = s[6];
        __m128i s8 = s[8], s10 = s[10], s12 = s[12], s14 = s[14];
        __m128i s16 = s[16], s18 = s[18], s20 = s[20], s22 = s[22];
        __m128i s24 = s[24], s26 = s[26], s28 = s[28], s30 = s[30];
        eo0 = A8(s2, 90, s6, 87, s10, 80, s14, 70, s18, 57, s22, 43, s26, 25, s30, 9);
        eo1 = A8(s2, 87, s6, 57, s10, 9, s14, -43, s18, -80, s22, -90, s26, -70, s30, -25);
        eo2 = A8(s2, 80, s6, 9, s10, -70, s14, -87, s18, -25, s22, 57, s26, 90, s30, 43);
        eo3 = A8(s2, 70, s6, -43, s10, -87, s14, 9, s18, 90, s22, 25, s26, -80, s30, -57);
        eo4 = A8(s2, 57, s6, -80, s10, -25, s14, 90, s18, -9, s22, -87, s26, 43, s30, 70);
        eo5 = A8(s2, 43, s6, -90, s10, 57, s14, 25, s18, -87, s22, 70, s26, 9, s30, -80);
        eo6 = A8(s2, 25, s6, -70, s10, 90, s14, -80, s18, 43, s22, 9, s26, -57, s30, 87);
        eo7 = A8(s2, 9, s6, -25, s10, 43, s14, -57, s18, 70, s22, -80, s26, 87, s30, -90);
        eeo0 = A4(s4, 89, s12, 75, s20, 50, s28, 18);
        eeo1 = A4(s4, 75, s12, -18, s20, -89, s28, -50);
        eeo2 = A4(s4, 50, s12, -89, s20, 18, s28, 75);
        eeo3 = A4(s4, 18, s12, -50, s20, 75, s28, -89);
        eeee0 = A2(s0, 64, s16, 64);
        eeee1 = A2(s0, 64, s16, -64);
        eeeo0 = A2(s8, 83, s24, 36);
        eeeo1 = A2(s8, 36, s24, -83);
        eee0 = hs_add(eeee0, eeeo0);
        eee1 = hs_add(eeee1, eeeo1);
        eee2 = hs_sub(eeee1, eeeo1);
        eee3 = hs_sub(eeee0, eeeo0);
        ee0 = hs_add(eee0, eeo0);
        ee1 = hs_add(eee1, eeo1);
        ee2 = hs_add(eee2, eeo2);
        ee3 = hs_add(eee3, eeo3);
        ee4 = hs_sub(eee3, eeo3);
        ee5 = hs_sub(eee2, eeo2);
        ee6 = hs_sub(eee1, eeo1);
        ee7 = hs_sub(eee0, eeo0);
        e0 = hs_add(ee0, eo0);
        e1 = hs_add(ee1, eo1);
        e2 = hs_add(ee2, eo2);
        e3 = hs_add(ee3, eo3);
        e4 = hs_add(ee4, eo4);
        e5 = hs_add(ee5, eo5);
        e6 = hs_add(ee6, eo6);
        e7 = hs_add(ee7, eo7);
        e8 = hs_sub(ee7, eo7);
        e9 = hs_sub(ee6, eo6);
        e10 = hs_sub(ee5, eo5);
        e11 = hs_sub(ee4, eo4);
        e12 = hs_sub(ee3, eo3);
        e13 = hs_sub(ee2, eo2);
        e14 = hs_sub(ee1, eo1);
        e15 = hs_sub(ee0, eo0);
        d[0] = hs_clip16_shift(hs_add(e0, o0), add, shift);
        d[1] = hs_clip16_shift(hs_add(e1, o1), add, shift);
        d[2] = hs_clip16_shift(hs_add(e2, o2), add, shift);
        d[3] = hs_clip16_shift(hs_add(e3, o3), add, shift);
        d[4] = hs_clip16_shift(hs_add(e4, o4), add, shift);
        d[5] = hs_clip16_shift(hs_add(e5, o5), add, shift);
        d[6] = hs_clip16_shift(hs_add(e6, o6), add, shift);
        d[7] = hs_clip16_shift(hs_add(e7, o7), add, shift);
        d[8] = hs_clip16_shift(hs_add(e8, o8), add, shift);
        d[9] = hs_clip16_shift(hs_add(e9, o9), add, shift);
        d[10] = hs_clip16_shift(hs_add(e10, o10), add, shift);
        d[11] = hs_clip16_shift(hs_add(e11, o11), add, shift);
        d[12] = hs_clip16_shift(hs_add(e12, o12), add, shift);
        d[13] = hs_clip16_shift(hs_add(e13, o13), add, shift);
        d[14] = hs_clip16_shift(hs_add(e14, o14), add, shift);
        d[15] = hs_clip16_shift(hs_add(e15, o15), add, shift);
        d[16] = hs_clip16_shift(hs_sub(e15, o15), add, shift);
        d[17] = hs_clip16_shift(hs_sub(e14, o14), add, shift);
        d[18] = hs_clip16_shift(hs_sub(e13, o13), add, shift);
        d[19] = hs_clip16_shift(hs_sub(e12, o12), add, shift);
        d[20] = hs_clip16_shift(hs_sub(e11, o11), add, shift);
        d[21] = hs_clip16_shift(hs_sub(e10, o10), add, shift);
        d[22] = hs_clip16_shift(hs_sub(e9, o9), add, shift);
        d[23] = hs_clip16_shift(hs_sub(e8, o8), add, shift);
        d[24] = hs_clip16_shift(hs_sub(e7, o7), add, shift);
        d[25] = hs_clip16_shift(hs_sub(e6, o6), add, shift);
        d[26] = hs_clip16_shift(hs_sub(e5, o5), add, shift);
        d[27] = hs_clip16_shift(hs_sub(e4, o4), add, shift);
        d[28] = hs_clip16_shift(hs_sub(e3, o3), add, shift);
        d[29] = hs_clip16_shift(hs_sub(e2, o2), add, shift);
        d[30] = hs_clip16_shift(hs_sub(e1, o1), add, shift);
        d[31] = hs_clip16_shift(hs_sub(e0, o0), add, shift);
    }
#undef A8
#undef A4
#undef A2
}

static int only_dc_n(const int16_t *c, int n)
{
    int i, nn = n * n;
    for (i = 1; i < nn; i++)
        if (c[i]) return 0;
    return 1;
}

static void dc_fill_n(int16_t *out, int n, int16_t dc, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t add1 = 1 << (shift1 - 1), add2 = 1 << (shift2 - 1);
    int32_t v1 = (64 * (int32_t)dc + add1) >> shift1;
    int16_t v;
    int i, nn = n * n;
    if (v1 < -32768) v1 = -32768;
    if (v1 > 32767) v1 = 32767;
    v1 = (64 * v1 + add2) >> shift2;
    if (v1 < -32768) v1 = -32768;
    if (v1 > 32767) v1 = 32767;
    v = (int16_t)v1;
    for (i = 0; i < nn; i++) out[i] = v;
}

int heic_simd_idct8(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t tmp[64];
    int col, row;
    __m128i s[8], d[8];
    if (!g_simd) return 0;
    if (only_dc_n(coeffs, 8)) {
        dc_fill_n(output, 8, coeffs[0], bit_depth);
        return 1;
    }
    memset(tmp, 0, sizeof(tmp));
    for (col = 0; col < 8; col += 4) {
        load4_cols_i16(coeffs, 8, col, s, 8);
        idct8_1d_x4(s, d, shift1);
        store4_cols_i32(tmp, 8, col, d, 8);
    }
    for (row = 0; row < 8; row += 4) {
        load4_rows_i32(tmp, 8, row, s, 8);
        idct8_1d_x4(s, d, shift2);
        store4_rows_i16(output, 8, row, d, 8);
    }
    return 1;
}

int heic_simd_idct16(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t *tmp = heic_idct_scratch_buf();
    int col, row;
    __m128i s[16], d[16];
    if (!g_simd) return 0;
    if (only_dc_n(coeffs, 16)) {
        dc_fill_n(output, 16, coeffs[0], bit_depth);
        return 1;
    }
    memset(tmp, 0, 256 * sizeof(int32_t));
    for (col = 0; col < 16; col += 4) {
        load4_cols_i16(coeffs, 16, col, s, 16);
        idct16_1d_x4(s, d, shift1);
        store4_cols_i32(tmp, 16, col, d, 16);
    }
    for (row = 0; row < 16; row += 4) {
        load4_rows_i32(tmp, 16, row, s, 16);
        idct16_1d_x4(s, d, shift2);
        store4_rows_i16(output, 16, row, d, 16);
    }
    return 1;
}

int heic_simd_idct32(const int16_t *coeffs, int16_t *output, int bit_depth)
{
    int shift1 = 7, shift2 = 20 - bit_depth;
    int32_t *tmp = heic_idct_scratch_buf();
    int col, row;
    __m128i s[32], d[32];
    if (!g_simd) return 0;
    if (only_dc_n(coeffs, 32)) {
        dc_fill_n(output, 32, coeffs[0], bit_depth);
        return 1;
    }
    memset(tmp, 0, 1024 * sizeof(int32_t));
    for (col = 0; col < 32; col += 4) {
        /* Skip all-zero column groups. */
        {
            int k, any = 0;
            for (k = 0; k < 32 && !any; k++) {
                const int16_t *p = coeffs + k * 32 + col;
                if (p[0] | p[1] | p[2] | p[3]) any = 1;
            }
            if (!any) continue;
        }
        load4_cols_i16(coeffs, 32, col, s, 32);
        idct32_1d_x4(s, d, shift1);
        store4_cols_i32(tmp, 32, col, d, 32);
    }
    for (row = 0; row < 32; row += 4) {
        load4_rows_i32(tmp, 32, row, s, 32);
        idct32_1d_x4(s, d, shift2);
        store4_rows_i16(output, 32, row, d, 32);
    }
    return 1;
}

/* residual: plane[i] = clip(plane[i] + res[i], 0, max_val) */
int heic_simd_add_residual(uint16_t *plane, int stride, int x0, int y0,
                           const int16_t *residual, int size, int max_val)
{
    int py, px;
    __m128i vzero;
    if (!g_simd) return 0;
    if (max_val > 65535) return 0;
    vzero = _mm_setzero_si128();
    for (py = 0; py < size; py++) {
        uint16_t *dst = plane + (y0 + py) * stride + x0;
        const int16_t *src = residual + py * size;
        px = 0;
        for (; px + 8 <= size; px += 8) {
            __m128i d = _mm_loadu_si128((const __m128i *)(dst + px));
            __m128i r = _mm_loadu_si128((const __m128i *)(src + px));
            /* Promote to 32-bit, add, clamp, pack. */
            __m128i d_lo = _mm_unpacklo_epi16(d, vzero);
            __m128i d_hi = _mm_unpackhi_epi16(d, vzero);
            __m128i r_lo = _mm_cvtepi16_epi32(r);
            __m128i r_hi = _mm_cvtepi16_epi32(_mm_srli_si128(r, 8));
            __m128i s_lo = _mm_add_epi32(d_lo, r_lo);
            __m128i s_hi = _mm_add_epi32(d_hi, r_hi);
            s_lo = _mm_max_epi32(s_lo, _mm_setzero_si128());
            s_hi = _mm_max_epi32(s_hi, _mm_setzero_si128());
            s_lo = _mm_min_epi32(s_lo, _mm_set1_epi32(max_val));
            s_hi = _mm_min_epi32(s_hi, _mm_set1_epi32(max_val));
            _mm_storeu_si128((__m128i *)(dst + px), _mm_packus_epi32(s_lo, s_hi));
        }
        for (; px < size; px++) {
            int32_t v = (int32_t)dst[px] + (int32_t)src[px];
            if (v < 0) v = 0;
            else if (v > max_val) v = max_val;
            dst[px] = (uint16_t)v;
        }
    }
    return 1;
}

/* Gather 4 LUT entries for indices taken from u16 plane samples (low 8 bits). */
static inline __m128i lut4_i32(const int32_t *tab, const uint16_t *p)
{
    return _mm_setr_epi32(tab[p[0] & 255], tab[p[1] & 255], tab[p[2] & 255],
                          tab[p[3] & 255]);
}

static inline __m128i lut420_i32(const int32_t *tab, const uint16_t *p, int phase)
{
    if (phase)
        return _mm_setr_epi32(tab[p[0] & 255], tab[p[1] & 255], tab[p[1] & 255],
                              tab[p[2] & 255]);
    return _mm_setr_epi32(tab[p[0] & 255], tab[p[0] & 255], tab[p[1] & 255],
                          tab[p[1] & 255]);
}

static inline __m128i clamp_u8_epi32(__m128i v)
{
    v = _mm_max_epi32(v, _mm_setzero_si128());
    v = _mm_min_epi32(v, _mm_set1_epi32(255));
    return v;
}

/* Pack 4 R, 4 G, 4 B (epi32 0..255) into 12 RGB bytes at dst via SSSE3. */
static inline void store_rgb4(__m128i r, __m128i g, __m128i b, uint8_t *dst)
{
    __m128i r8 = _mm_packus_epi16(_mm_packus_epi32(r, r), _mm_setzero_si128());
    __m128i g8 = _mm_packus_epi16(_mm_packus_epi32(g, g), _mm_setzero_si128());
    __m128i b8 = _mm_packus_epi16(_mm_packus_epi32(b, b), _mm_setzero_si128());
    /* Concatenate rrrr|gggg|bbbb into one register, then shuffle to RGBRGB... */
    __m128i rgb = _mm_set_epi32(0, _mm_cvtsi128_si32(b8), _mm_cvtsi128_si32(g8),
                                _mm_cvtsi128_si32(r8));
    static const char k_shuf[16] = {0, 4, 8, 1, 5, 9, 2, 6, 10, 3, 7, 11, -1, -1, -1, -1};
    __m128i out = _mm_shuffle_epi8(rgb, _mm_loadu_si128((const __m128i *)k_shuf));
    _mm_storel_epi64((__m128i *)dst, out);
    {
        unsigned t = (unsigned)_mm_extract_epi32(out, 2);
        dst[8] = (uint8_t)t;
        dst[9] = (uint8_t)(t >> 8);
        dst[10] = (uint8_t)(t >> 16);
        dst[11] = (uint8_t)(t >> 24);
    }
}

int heic_simd_ycc_444_row(const uint16_t *yp, const uint16_t *cbp, const uint16_t *crp,
                          uint8_t *row, int w, int full, const int32_t yv[256],
                          const int32_t cr_r[256], const int32_t cb_g[256],
                          const int32_t cr_g[256], const int32_t cb_b[256])
{
    int x;
    if (!g_simd || w < 4) return 0;
    if (full) {
        __m128i round = _mm_set1_epi32(128);
        __m128i mask = _mm_set1_epi16(255);
        __m128i center = _mm_set1_epi32(128);
        __m128i k_cr_r = _mm_set1_epi32(cr_r[129]);
        __m128i k_cb_g = _mm_set1_epi32(cb_g[129]);
        __m128i k_cr_g = _mm_set1_epi32(cr_g[129]);
        __m128i k_cb_b = _mm_set1_epi32(cb_b[129]);
        for (x = 0; x + 4 <= w; x += 4) {
            __m128i Y = _mm_cvtepu16_epi32(
                _mm_and_si128(_mm_loadl_epi64((const __m128i *)(yp + x)), mask));
            __m128i Cb = _mm_sub_epi32(
                _mm_cvtepu16_epi32(
                    _mm_and_si128(_mm_loadl_epi64((const __m128i *)(cbp + x)), mask)),
                center);
            __m128i Cr = _mm_sub_epi32(
                _mm_cvtepu16_epi32(
                    _mm_and_si128(_mm_loadl_epi64((const __m128i *)(crp + x)), mask)),
                center);
            __m128i CrR = _mm_mullo_epi32(Cr, k_cr_r);
            __m128i CbG = _mm_mullo_epi32(Cb, k_cb_g);
            __m128i CrG = _mm_mullo_epi32(Cr, k_cr_g);
            __m128i CbB = _mm_mullo_epi32(Cb, k_cb_b);
            __m128i rr = clamp_u8_epi32(
                _mm_add_epi32(Y, _mm_srai_epi32(_mm_add_epi32(CrR, round), 8)));
            __m128i gg = clamp_u8_epi32(_mm_add_epi32(
                Y, _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(CbG, CrG), round), 8)));
            __m128i bb = clamp_u8_epi32(
                _mm_add_epi32(Y, _mm_srai_epi32(_mm_add_epi32(CbB, round), 8)));
            store_rgb4(rr, gg, bb, row + x * 3);
        }
    } else {
        __m128i round = _mm_set1_epi32(4096);
        for (x = 0; x + 4 <= w; x += 4) {
            __m128i Y = lut4_i32(yv, yp + x);
            __m128i CrR = lut4_i32(cr_r, crp + x);
            __m128i CbG = lut4_i32(cb_g, cbp + x);
            __m128i CrG = lut4_i32(cr_g, crp + x);
            __m128i CbB = lut4_i32(cb_b, cbp + x);
            __m128i rr = clamp_u8_epi32(
                _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(Y, CrR), round), 13));
            __m128i gg = clamp_u8_epi32(_mm_srai_epi32(
                _mm_add_epi32(_mm_add_epi32(_mm_add_epi32(Y, CbG), CrG), round), 13));
            __m128i bb = clamp_u8_epi32(
                _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(Y, CbB), round), 13));
            store_rgb4(rr, gg, bb, row + x * 3);
        }
    }
    /* Tail */
    for (; x < w; x++) {
        int Y = (int)yp[x] & 255, Cb = (int)cbp[x] & 255, Cr = (int)crp[x] & 255;
        int rr, gg, bb;
        if (full) {
            rr = yv[Y] + ((cr_r[Cr] + 128) >> 8);
            gg = yv[Y] + ((cb_g[Cb] + cr_g[Cr] + 128) >> 8);
            bb = yv[Y] + ((cb_b[Cb] + 128) >> 8);
        } else {
            rr = (yv[Y] + cr_r[Cr] + 4096) >> 13;
            gg = (yv[Y] + cb_g[Cb] + cr_g[Cr] + 4096) >> 13;
            bb = (yv[Y] + cb_b[Cb] + 4096) >> 13;
        }
        if (rr < 0) rr = 0;
        else if (rr > 255) rr = 255;
        if (gg < 0) gg = 0;
        else if (gg > 255) gg = 255;
        if (bb < 0) bb = 0;
        else if (bb > 255) bb = 255;
        row[x * 3 + 0] = (uint8_t)rr;
        row[x * 3 + 1] = (uint8_t)gg;
        row[x * 3 + 2] = (uint8_t)bb;
    }
    return 1;
}

/* Expand 2 or 3 subsampled chroma samples into 4 epi32 lanes with 4:2:0 phase. */
static inline __m128i chroma420_epi32(const uint16_t *p, int phase)
{
    __m128i mask = _mm_set1_epi16(255);
    __m128i center = _mm_set1_epi32(128);
    int a = (int)(p[0] & 255), b = (int)(p[1] & 255), c = (int)(p[2] & 255);
    __m128i v;
    (void)mask;
    if (phase)
        v = _mm_setr_epi32(a, b, b, c);
    else
        v = _mm_setr_epi32(a, a, b, b);
    return _mm_sub_epi32(v, center);
}

int heic_simd_ycc_420_row(const uint16_t *yp, const uint16_t *cbp, const uint16_t *crp,
                          uint8_t *row, int w, int phase, int full,
                          const int32_t yv[256], const int32_t cr_r[256],
                          const int32_t cb_g[256], const int32_t cr_g[256],
                          const int32_t cb_b[256])
{
    int x;
    if (!g_simd || w < 4) return 0;
    if (full) {
        /* Direct fixed-point like 4:4:4 full path — avoid 5 scalar LUT gathers. */
        __m128i round = _mm_set1_epi32(128);
        __m128i mask = _mm_set1_epi16(255);
        __m128i k_cr_r = _mm_set1_epi32(cr_r[129]); /* coeff * 1 */
        __m128i k_cb_g = _mm_set1_epi32(cb_g[129]);
        __m128i k_cr_g = _mm_set1_epi32(cr_g[129]);
        __m128i k_cb_b = _mm_set1_epi32(cb_b[129]);
        for (x = 0; x + 4 <= w; x += 4) {
            const uint16_t *cb = cbp + ((phase + x) >> 1);
            const uint16_t *cr = crp + ((phase + x) >> 1);
            __m128i Y = _mm_cvtepu16_epi32(
                _mm_and_si128(_mm_loadl_epi64((const __m128i *)(yp + x)), mask));
            __m128i Cb = chroma420_epi32(cb, phase);
            __m128i Cr = chroma420_epi32(cr, phase);
            __m128i CrR = _mm_mullo_epi32(Cr, k_cr_r);
            __m128i CbG = _mm_mullo_epi32(Cb, k_cb_g);
            __m128i CrG = _mm_mullo_epi32(Cr, k_cr_g);
            __m128i CbB = _mm_mullo_epi32(Cb, k_cb_b);
            __m128i rr = clamp_u8_epi32(
                _mm_add_epi32(Y, _mm_srai_epi32(_mm_add_epi32(CrR, round), 8)));
            __m128i gg = clamp_u8_epi32(_mm_add_epi32(
                Y, _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(CbG, CrG), round), 8)));
            __m128i bb = clamp_u8_epi32(
                _mm_add_epi32(Y, _mm_srai_epi32(_mm_add_epi32(CbB, round), 8)));
            store_rgb4(rr, gg, bb, row + x * 3);
        }
    } else {
        __m128i round = _mm_set1_epi32(4096);
        for (x = 0; x + 4 <= w; x += 4) {
            const uint16_t *cb = cbp + ((phase + x) >> 1);
            const uint16_t *cr = crp + ((phase + x) >> 1);
            __m128i Y = lut4_i32(yv, yp + x);
            __m128i CrR = lut420_i32(cr_r, cr, phase);
            __m128i CbG = lut420_i32(cb_g, cb, phase);
            __m128i CrG = lut420_i32(cr_g, cr, phase);
            __m128i CbB = lut420_i32(cb_b, cb, phase);
            __m128i rr = clamp_u8_epi32(
                _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(Y, CrR), round), 13));
            __m128i gg = clamp_u8_epi32(_mm_srai_epi32(
                _mm_add_epi32(_mm_add_epi32(_mm_add_epi32(Y, CbG), CrG), round), 13));
            __m128i bb = clamp_u8_epi32(
                _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(Y, CbB), round), 13));
            store_rgb4(rr, gg, bb, row + x * 3);
        }
    }
    for (; x < w; x++) {
        int cx = (phase + x) >> 1;
        int Y = (int)yp[x] & 255, Cb = (int)cbp[cx] & 255, Cr = (int)crp[cx] & 255;
        int rr, gg, bb;
        if (full) {
            rr = yv[Y] + ((cr_r[Cr] + 128) >> 8);
            gg = yv[Y] + ((cb_g[Cb] + cr_g[Cr] + 128) >> 8);
            bb = yv[Y] + ((cb_b[Cb] + 128) >> 8);
        } else {
            rr = (yv[Y] + cr_r[Cr] + 4096) >> 13;
            gg = (yv[Y] + cb_g[Cb] + cr_g[Cr] + 4096) >> 13;
            bb = (yv[Y] + cb_b[Cb] + 4096) >> 13;
        }
        if (rr < 0) rr = 0;
        else if (rr > 255) rr = 255;
        if (gg < 0) gg = 0;
        else if (gg > 255) gg = 255;
        if (bb < 0) bb = 0;
        else if (bb > 255) bb = 255;
        row[x * 3 + 0] = (uint8_t)rr;
        row[x * 3 + 1] = (uint8_t)gg;
        row[x * 3 + 2] = (uint8_t)bb;
    }
    return 1;
}

/* Chroma weak filter on 4 samples along an edge.
 * base_q0 = address of first q0 sample; next samples are +across (1 for horiz
 * edge along x, or stride for vert edge along y). p1,p0,q0,q1 are at
 * base-2*across_step, base-across_step, base, base+across_step where
 * across_step is 1 for vertical edges (samples side-by-side) and stride for
 * horizontal edges. */
int heic_simd_chroma_edge4(uint16_t *plane, int stride, size_t base_q0, int across,
                           int tc, int max_val, int along_is_stride)
{
    size_t along = along_is_stride ? (size_t)stride : 1;
    size_t ac = (size_t)across;
    __m128i p1, p0, q0, q1, delta, p0n, q0n, vtc, vmtc, vmax;
    size_t b0, b1, b2, b3;
    if (!g_simd || tc <= 0) return 0;
    b0 = base_q0;
    b1 = base_q0 + along;
    b2 = base_q0 + 2 * along;
    b3 = base_q0 + 3 * along;
    /* Load p1,p0,q0,q1 for 4 edge samples. */
    p1 = _mm_setr_epi32(plane[b0 - 2 * ac], plane[b1 - 2 * ac], plane[b2 - 2 * ac],
                        plane[b3 - 2 * ac]);
    p0 = _mm_setr_epi32(plane[b0 - ac], plane[b1 - ac], plane[b2 - ac], plane[b3 - ac]);
    q0 = _mm_setr_epi32(plane[b0], plane[b1], plane[b2], plane[b3]);
    q1 = _mm_setr_epi32(plane[b0 + ac], plane[b1 + ac], plane[b2 + ac], plane[b3 + ac]);
    /* delta = ((q0 - p0)*4 + p1 - q1 + 4) >> 3 */
    delta = _mm_sub_epi32(q0, p0);
    delta = _mm_slli_epi32(delta, 2);
    delta = _mm_add_epi32(delta, p1);
    delta = _mm_sub_epi32(delta, q1);
    delta = _mm_add_epi32(delta, _mm_set1_epi32(4));
    delta = _mm_srai_epi32(delta, 3);
    vtc = _mm_set1_epi32(tc);
    vmtc = _mm_set1_epi32(-tc);
    delta = _mm_min_epi32(delta, vtc);
    delta = _mm_max_epi32(delta, vmtc);
    vmax = _mm_set1_epi32(max_val);
    p0n = _mm_add_epi32(p0, delta);
    q0n = _mm_sub_epi32(q0, delta);
    p0n = _mm_max_epi32(p0n, _mm_setzero_si128());
    q0n = _mm_max_epi32(q0n, _mm_setzero_si128());
    p0n = _mm_min_epi32(p0n, vmax);
    q0n = _mm_min_epi32(q0n, vmax);
    {
        int32_t pn[4], qn[4];
        _mm_storeu_si128((__m128i *)pn, p0n);
        _mm_storeu_si128((__m128i *)qn, q0n);
        plane[b0 - ac] = (uint16_t)pn[0];
        plane[b1 - ac] = (uint16_t)pn[1];
        plane[b2 - ac] = (uint16_t)pn[2];
        plane[b3 - ac] = (uint16_t)pn[3];
        plane[b0] = (uint16_t)qn[0];
        plane[b1] = (uint16_t)qn[1];
        plane[b2] = (uint16_t)qn[2];
        plane[b3] = (uint16_t)qn[3];
    }
    return 1;
}

static inline __m128i load4_along(const uint16_t *plane, size_t base, size_t along)
{
    return _mm_setr_epi32(plane[base], plane[base + along], plane[base + 2 * along],
                          plane[base + 3 * along]);
}

static inline void store4_along(uint16_t *plane, size_t base, size_t along, __m128i v)
{
    int32_t t[4];
    _mm_storeu_si128((__m128i *)t, v);
    plane[base] = (uint16_t)t[0];
    plane[base + along] = (uint16_t)t[1];
    plane[base + 2 * along] = (uint16_t)t[2];
    plane[base + 3 * along] = (uint16_t)t[3];
}

static inline __m128i clamp_i32(__m128i v, int lo, int hi)
{
    v = _mm_max_epi32(v, _mm_set1_epi32(lo));
    v = _mm_min_epi32(v, _mm_set1_epi32(hi));
    return v;
}

int heic_simd_luma_filter4(uint16_t *plane, size_t base_p, size_t base_q,
                           size_t step_along, size_t step_across, int strong, int d_ep,
                           int d_eq, int tc, int max_val)
{
    __m128i p0, p1, p2, p3, q0, q1, q2, q3;
    if (!g_simd || tc <= 0) return 0;
    p0 = load4_along(plane, base_p, step_along);
    p1 = load4_along(plane, base_p - step_across, step_along);
    p2 = load4_along(plane, base_p - 2 * step_across, step_along);
    q0 = load4_along(plane, base_q, step_along);
    q1 = load4_along(plane, base_q + step_across, step_along);
    q2 = load4_along(plane, base_q + 2 * step_across, step_along);

    if (strong) {
        int tc2 = 2 * tc;
        __m128i four = _mm_set1_epi32(4);
        __m128i two = _mm_set1_epi32(2);
        __m128i p0f, p1f, p2f, q0f, q1f, q2f;
        p3 = load4_along(plane, base_p - 3 * step_across, step_along);
        q3 = load4_along(plane, base_q + 3 * step_across, step_along);
        /* p0' = (p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4) >> 3 */
        p0f = _mm_add_epi32(p2, _mm_slli_epi32(p1, 1));
        p0f = _mm_add_epi32(p0f, _mm_slli_epi32(p0, 1));
        p0f = _mm_add_epi32(p0f, _mm_slli_epi32(q0, 1));
        p0f = _mm_add_epi32(p0f, q1);
        p0f = _mm_srai_epi32(_mm_add_epi32(p0f, four), 3);
        /* tc clip: clamp(p0f, p0-tc2, p0+tc2) then 0..max */
        p0f = _mm_min_epi32(p0f, _mm_add_epi32(p0, _mm_set1_epi32(tc2)));
        p0f = _mm_max_epi32(p0f, _mm_sub_epi32(p0, _mm_set1_epi32(tc2)));
        p0f = clamp_i32(p0f, 0, max_val);

        p1f = _mm_add_epi32(_mm_add_epi32(p2, p1), _mm_add_epi32(p0, q0));
        p1f = _mm_srai_epi32(_mm_add_epi32(p1f, two), 2);
        p1f = _mm_min_epi32(p1f, _mm_add_epi32(p1, _mm_set1_epi32(tc2)));
        p1f = _mm_max_epi32(p1f, _mm_sub_epi32(p1, _mm_set1_epi32(tc2)));
        p1f = clamp_i32(p1f, 0, max_val);

        p2f = _mm_add_epi32(_mm_slli_epi32(p3, 1), _mm_add_epi32(_mm_mullo_epi32(p2, _mm_set1_epi32(3)), p1));
        p2f = _mm_add_epi32(p2f, _mm_add_epi32(p0, q0));
        p2f = _mm_srai_epi32(_mm_add_epi32(p2f, four), 3);
        p2f = _mm_min_epi32(p2f, _mm_add_epi32(p2, _mm_set1_epi32(tc2)));
        p2f = _mm_max_epi32(p2f, _mm_sub_epi32(p2, _mm_set1_epi32(tc2)));
        p2f = clamp_i32(p2f, 0, max_val);

        q0f = _mm_add_epi32(p1, _mm_slli_epi32(p0, 1));
        q0f = _mm_add_epi32(q0f, _mm_slli_epi32(q0, 1));
        q0f = _mm_add_epi32(q0f, _mm_slli_epi32(q1, 1));
        q0f = _mm_add_epi32(q0f, q2);
        q0f = _mm_srai_epi32(_mm_add_epi32(q0f, four), 3);
        q0f = _mm_min_epi32(q0f, _mm_add_epi32(q0, _mm_set1_epi32(tc2)));
        q0f = _mm_max_epi32(q0f, _mm_sub_epi32(q0, _mm_set1_epi32(tc2)));
        q0f = clamp_i32(q0f, 0, max_val);

        q1f = _mm_add_epi32(_mm_add_epi32(p0, q0), _mm_add_epi32(q1, q2));
        q1f = _mm_srai_epi32(_mm_add_epi32(q1f, two), 2);
        q1f = _mm_min_epi32(q1f, _mm_add_epi32(q1, _mm_set1_epi32(tc2)));
        q1f = _mm_max_epi32(q1f, _mm_sub_epi32(q1, _mm_set1_epi32(tc2)));
        q1f = clamp_i32(q1f, 0, max_val);

        q2f = _mm_add_epi32(p0, q0);
        q2f = _mm_add_epi32(q2f, q1);
        q2f = _mm_add_epi32(q2f, _mm_mullo_epi32(q2, _mm_set1_epi32(3)));
        q2f = _mm_add_epi32(q2f, _mm_slli_epi32(q3, 1));
        q2f = _mm_srai_epi32(_mm_add_epi32(q2f, four), 3);
        q2f = _mm_min_epi32(q2f, _mm_add_epi32(q2, _mm_set1_epi32(tc2)));
        q2f = _mm_max_epi32(q2f, _mm_sub_epi32(q2, _mm_set1_epi32(tc2)));
        q2f = clamp_i32(q2f, 0, max_val);

        store4_along(plane, base_p, step_along, p0f);
        store4_along(plane, base_p - step_across, step_along, p1f);
        store4_along(plane, base_p - 2 * step_across, step_along, p2f);
        store4_along(plane, base_q, step_along, q0f);
        store4_along(plane, base_q + step_across, step_along, q1f);
        store4_along(plane, base_q + 2 * step_across, step_along, q2f);
    } else {
        /* delta = (9*(q0-p0) - 3*(q1-p1) + 8) >> 4 */
        __m128i diff0 = _mm_sub_epi32(q0, p0);
        __m128i diff1 = _mm_sub_epi32(q1, p1);
        __m128i delta =
            _mm_sub_epi32(_mm_mullo_epi32(diff0, _mm_set1_epi32(9)),
                          _mm_mullo_epi32(diff1, _mm_set1_epi32(3)));
        __m128i absd, mask, thr;
        delta = _mm_srai_epi32(_mm_add_epi32(delta, _mm_set1_epi32(8)), 4);
        absd = _mm_abs_epi32(delta); /* SSSE3 */
        thr = _mm_set1_epi32(10 * tc);
        mask = _mm_cmplt_epi32(absd, thr); /* absd < 10*tc */
        delta = clamp_i32(delta, -tc, tc);
        delta = _mm_and_si128(delta, mask);
        {
            __m128i p0n = clamp_i32(_mm_add_epi32(p0, delta), 0, max_val);
            __m128i q0n = clamp_i32(_mm_sub_epi32(q0, delta), 0, max_val);
            /* Only write where mask; blend with original */
            p0n = _mm_blendv_epi8(p0, p0n, mask);
            q0n = _mm_blendv_epi8(q0, q0n, mask);
            store4_along(plane, base_p, step_along, p0n);
            store4_along(plane, base_q, step_along, q0n);
            if (d_ep) {
                int tch = tc >> 1;
                __m128i dp = _mm_srai_epi32(_mm_add_epi32(p2, p0), 1); /* (p2+p0)>>1 approx; need +1 */
                dp = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(p2, p0), _mm_set1_epi32(1)), 1);
                dp = _mm_sub_epi32(dp, p1);
                dp = _mm_add_epi32(dp, delta);
                dp = _mm_srai_epi32(dp, 1);
                dp = clamp_i32(dp, -tch, tch);
                dp = _mm_and_si128(dp, mask);
                {
                    __m128i p1n = clamp_i32(_mm_add_epi32(p1, dp), 0, max_val);
                    p1n = _mm_blendv_epi8(p1, p1n, mask);
                    store4_along(plane, base_p - step_across, step_along, p1n);
                }
            }
            if (d_eq) {
                int tch = tc >> 1;
                __m128i dq =
                    _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(q2, q0), _mm_set1_epi32(1)), 1);
                dq = _mm_sub_epi32(dq, q1);
                dq = _mm_sub_epi32(dq, delta);
                dq = _mm_srai_epi32(dq, 1);
                dq = clamp_i32(dq, -tch, tch);
                dq = _mm_and_si128(dq, mask);
                {
                    __m128i q1n = clamp_i32(_mm_add_epi32(q1, dq), 0, max_val);
                    q1n = _mm_blendv_epi8(q1, q1n, mask);
                    store4_along(plane, base_q + step_across, step_along, q1n);
                }
            }
        }
    }
    return 1;
}

int heic_simd_dequant(int16_t *coeffs, int n, int32_t combined, int shift)
{
    int i;
    __m128i c, add;
    if (!g_simd || shift < 0 || combined > 65536 || n < 8) return 0;
    c = _mm_set1_epi32(combined);
    add = shift > 0 ? _mm_set1_epi32(1 << (shift - 1)) : _mm_setzero_si128();
    for (i = 0; i + 8 <= n; i += 8) {
        __m128i v = _mm_loadu_si128((const __m128i *)(coeffs + i));
        __m128i a = _mm_cvtepi16_epi32(v);
        __m128i b = _mm_cvtepi16_epi32(_mm_srli_si128(v, 8));
        a = _mm_srai_epi32(_mm_add_epi32(_mm_mullo_epi32(a, c), add), shift);
        b = _mm_srai_epi32(_mm_add_epi32(_mm_mullo_epi32(b, c), add), shift);
        a = clamp_i32(a, -32768, 32767);
        b = clamp_i32(b, -32768, 32767);
        _mm_storeu_si128((__m128i *)(coeffs + i), _mm_packs_epi32(a, b));
    }
    for (; i < n; i++) {
        int32_t v = ((int32_t)coeffs[i] * combined + (shift > 0 ? (1 << (shift - 1)) : 0)) >> shift;
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        coeffs[i] = (int16_t)v;
    }
    return 1;
}

int heic_simd_intra_ang_row(uint16_t *dst, const int32_t *ref, int n, int a, int b,
                            int max_val)
{
    int i;
    __m128i va, vb, round, vmax;
    if (!g_simd || n < 4) return 0;
    va = _mm_set1_epi32(a);
    vb = _mm_set1_epi32(b);
    round = _mm_set1_epi32(16);
    vmax = _mm_set1_epi32(max_val);
    for (i = 0; i + 4 <= n; i += 4) {
        __m128i r0 = _mm_loadu_si128((const __m128i *)(ref + i));
        __m128i r1 = _mm_loadu_si128((const __m128i *)(ref + i + 1));
        __m128i pred =
            _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(_mm_mullo_epi32(va, r0),
                                                       _mm_mullo_epi32(vb, r1)),
                                         round),
                           5);
        pred = clamp_i32(pred, 0, max_val);
        {
            int32_t t[4];
            _mm_storeu_si128((__m128i *)t, pred);
            dst[i] = (uint16_t)t[0];
            dst[i + 1] = (uint16_t)t[1];
            dst[i + 2] = (uint16_t)t[2];
            dst[i + 3] = (uint16_t)t[3];
        }
        (void)vmax;
    }
    for (; i < n; i++) {
        int32_t pred = (a * ref[i] + b * ref[i + 1] + 16) >> 5;
        if (pred < 0) pred = 0;
        if (pred > max_val) pred = max_val;
        dst[i] = (uint16_t)pred;
    }
    return 1;
}

int heic_simd_u16_to_i32_avail(const uint16_t *src, int32_t *border, int *avail, int n)
{
    int i;
    if (!g_simd || n < 4) return 0;
    for (i = 0; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadl_epi64((const __m128i *)(src + i)); /* 4×u16 */
        __m128i z = _mm_setzero_si128();
        __m128i lo = _mm_unpacklo_epi16(v, z); /* 4×i32 */
        _mm_storeu_si128((__m128i *)(border + i), lo);
        avail[i] = avail[i + 1] = avail[i + 2] = avail[i + 3] = 1;
    }
    for (; i < n; i++) {
        border[i] = src[i];
        avail[i] = 1;
    }
    return 1;
}

int heic_simd_border_top_ext(const uint16_t *src, int32_t *border, int *avail, int n)
{
    int i, count = 0;
    __m128i uninit = _mm_set1_epi16(-1); /* 0xFFFF as i16 */
    if (!g_simd || n < 4) return 0;
    for (i = 0; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadl_epi64((const __m128i *)(src + i));
        __m128i ok = _mm_cmpeq_epi16(v, uninit); /* FFFF if UNINIT */
        ok = _mm_xor_si128(ok, _mm_set1_epi16(-1)); /* FFFF if valid */
        {
            int32_t t[4];
            int16_t s[4], m[4];
            _mm_storel_epi64((__m128i *)s, v);
            _mm_storel_epi64((__m128i *)m, ok);
            t[0] = s[0];
            t[1] = s[1];
            t[2] = s[2];
            t[3] = s[3];
            if (m[0]) {
                border[i] = t[0];
                avail[i] = 1;
                count++;
            }
            if (m[1]) {
                border[i + 1] = t[1];
                avail[i + 1] = 1;
                count++;
            }
            if (m[2]) {
                border[i + 2] = t[2];
                avail[i + 2] = 1;
                count++;
            }
            if (m[3]) {
                border[i + 3] = t[3];
                avail[i + 3] = 1;
                count++;
            }
        }
    }
    for (; i < n; i++) {
        if (src[i] != HEIC_UNINIT_SAMPLE) {
            border[i] = src[i];
            avail[i] = 1;
            count++;
        }
    }
    return count >= 0 ? 1 : 0; /* always "handled" when simd on */
}

int heic_simd_intra_ang_row_var(uint16_t *dst, const int32_t *ref, int n, int row_base,
                                int32_t angle, int max_val)
{
    int px;
    if (!g_simd || n < 4) return 0;
    /* Process 4 pixels; each has its own i_idx / i_fact from (px+1)*angle. */
    for (px = 0; px + 4 <= n; px += 4) {
        int k;
        int32_t pred4[4];
        for (k = 0; k < 4; k++) {
            int p = px + k;
            int32_t i_idx = ((p + 1) * angle) >> 5;
            int32_t i_fact = ((p + 1) * angle) & 31;
            int idx = row_base + (int)i_idx;
            int32_t pred;
            if (i_fact != 0)
                pred = ((32 - i_fact) * ref[idx] + i_fact * ref[idx + 1] + 16) >> 5;
            else
                pred = ref[idx];
            pred4[k] = pred;
        }
        {
            __m128i p = _mm_loadu_si128((const __m128i *)pred4);
            p = clamp_i32(p, 0, max_val);
            _mm_storeu_si128((__m128i *)pred4, p);
            dst[px] = (uint16_t)pred4[0];
            dst[px + 1] = (uint16_t)pred4[1];
            dst[px + 2] = (uint16_t)pred4[2];
            dst[px + 3] = (uint16_t)pred4[3];
        }
    }
    for (; px < n; px++) {
        int32_t i_idx = ((px + 1) * angle) >> 5;
        int32_t i_fact = ((px + 1) * angle) & 31;
        int idx = row_base + (int)i_idx;
        int32_t pred;
        if (i_fact != 0)
            pred = ((32 - i_fact) * ref[idx] + i_fact * ref[idx + 1] + 16) >> 5;
        else
            pred = ref[idx];
        if (pred < 0) pred = 0;
        if (pred > max_val) pred = max_val;
        dst[px] = (uint16_t)pred;
    }
    return 1;
}

/* sign(a): +1 / 0 / -1 via compares (no branch). */
static inline __m128i isign4(__m128i a)
{
    __m128i gt = _mm_cmpgt_epi32(a, _mm_setzero_si128());
    __m128i lt = _mm_cmplt_epi32(a, _mm_setzero_si128());
    return _mm_or_si128(_mm_and_si128(gt, _mm_set1_epi32(1)),
                        _mm_and_si128(lt, _mm_set1_epi32(-1)));
}

int heic_simd_sao_band_row(uint16_t *row, int x0, int x1, int band_shift,
                           const int16_t band_table[32], int max_val)
{
    int x;
    if (!g_simd || x1 - x0 < 8) return 0;
    /* 8-wide: load samples, compute band indices, apply offsets. */
    for (x = x0; x + 8 <= x1; x += 8) {
        __m128i v = _mm_loadu_si128((const __m128i *)(row + x));
        int16_t s[8];
        int k;
        _mm_storeu_si128((__m128i *)s, v);
        for (k = 0; k < 8; k++) {
            int sample = (int)(uint16_t)s[k];
            int band, offset, out;
            if (sample > max_val) sample = max_val;
            band = sample >> band_shift;
            offset = (int)band_table[band & 31];
            if (!offset) continue;
            out = sample + offset;
            if (out < 0) out = 0;
            else if (out > max_val) out = max_val;
            s[k] = (int16_t)out;
        }
        _mm_storeu_si128((__m128i *)(row + x), _mm_loadu_si128((const __m128i *)s));
    }
    for (; x < x1; x++) {
        int sample = (int)row[x];
        int band, offset;
        if (sample > max_val) sample = max_val;
        band = sample >> band_shift;
        offset = (int)band_table[band & 31];
        if (offset) {
            int v = sample + offset;
            if (v < 0) v = 0;
            else if (v > max_val) v = max_val;
            row[x] = (uint16_t)v;
        }
    }
    return 1;
}

int heic_simd_sao_edge_h_row(const uint16_t *srow, uint16_t *drow, int x0, int x1,
                             const int offset_table[5], int max_val)
{
    int x;
    if (!g_simd || x1 - x0 < 4) return 0;
    for (x = x0; x + 4 <= x1; x += 4) {
        __m128i s = _mm_setr_epi32(srow[x], srow[x + 1], srow[x + 2], srow[x + 3]);
        __m128i n0 = _mm_setr_epi32(srow[x - 1], srow[x], srow[x + 1], srow[x + 2]);
        __m128i n1 = _mm_setr_epi32(srow[x + 1], srow[x + 2], srow[x + 3], srow[x + 4]);
        __m128i e = _mm_add_epi32(_mm_set1_epi32(2),
                                  _mm_add_epi32(isign4(_mm_sub_epi32(s, n0)),
                                                isign4(_mm_sub_epi32(s, n1))));
        int32_t ei[4], si[4];
        int k;
        _mm_storeu_si128((__m128i *)ei, e);
        _mm_storeu_si128((__m128i *)si, s);
        for (k = 0; k < 4; k++) {
            int off = offset_table[ei[k]];
            if (off) {
                int v = si[k] + off;
                if (v < 0) v = 0;
                else if (v > max_val) v = max_val;
                drow[x + k] = (uint16_t)v;
            }
        }
    }
    for (; x < x1; x++) {
        int sample = (int)srow[x];
        int edge_idx = 2 + (sample > srow[x - 1] ? 1 : sample < srow[x - 1] ? -1 : 0) +
                       (sample > srow[x + 1] ? 1 : sample < srow[x + 1] ? -1 : 0);
        int off = offset_table[edge_idx];
        if (off) {
            int v = sample + off;
            if (v < 0) v = 0;
            else if (v > max_val) v = max_val;
            drow[x] = (uint16_t)v;
        }
    }
    return 1;
}

int heic_simd_sao_edge_v_row(const uint16_t *src, uint16_t *dst, int stride, int y,
                             int x0, int x1, const int offset_table[5], int max_val)
{
    int x;
    const uint16_t *srow = src + (size_t)y * (size_t)stride;
    const uint16_t *up = src + (size_t)(y - 1) * (size_t)stride;
    const uint16_t *dn = src + (size_t)(y + 1) * (size_t)stride;
    uint16_t *drow = dst + (size_t)y * (size_t)stride;
    if (!g_simd || x1 - x0 < 4) return 0;
    for (x = x0; x + 4 <= x1; x += 4) {
        __m128i s = _mm_setr_epi32(srow[x], srow[x + 1], srow[x + 2], srow[x + 3]);
        __m128i n0 = _mm_setr_epi32(up[x], up[x + 1], up[x + 2], up[x + 3]);
        __m128i n1 = _mm_setr_epi32(dn[x], dn[x + 1], dn[x + 2], dn[x + 3]);
        __m128i e = _mm_add_epi32(_mm_set1_epi32(2),
                                  _mm_add_epi32(isign4(_mm_sub_epi32(s, n0)),
                                                isign4(_mm_sub_epi32(s, n1))));
        int32_t ei[4], si[4];
        int k;
        _mm_storeu_si128((__m128i *)ei, e);
        _mm_storeu_si128((__m128i *)si, s);
        for (k = 0; k < 4; k++) {
            int off = offset_table[ei[k]];
            if (off) {
                int v = si[k] + off;
                if (v < 0) v = 0;
                else if (v > max_val) v = max_val;
                drow[x + k] = (uint16_t)v;
            }
        }
    }
    for (; x < x1; x++) {
        int sample = (int)srow[x];
        int edge_idx = 2 + (sample > up[x] ? 1 : sample < up[x] ? -1 : 0) +
                       (sample > dn[x] ? 1 : sample < dn[x] ? -1 : 0);
        int off = offset_table[edge_idx];
        if (off) {
            int v = sample + off;
            if (v < 0) v = 0;
            else if (v > max_val) v = max_val;
            drow[x] = (uint16_t)v;
        }
    }
    return 1;
}

#else /* !HEIC_X86 */

int heic_simd_idct8(const int16_t *c, int16_t *o, int bd)
{
    (void)c;
    (void)o;
    (void)bd;
    return 0;
}
int heic_simd_idct16(const int16_t *c, int16_t *o, int bd)
{
    (void)c;
    (void)o;
    (void)bd;
    return 0;
}
int heic_simd_idct32(const int16_t *c, int16_t *o, int bd)
{
    (void)c;
    (void)o;
    (void)bd;
    return 0;
}
int heic_simd_add_residual(uint16_t *p, int s, int x, int y, const int16_t *r, int n,
                           int m)
{
    (void)p;
    (void)s;
    (void)x;
    (void)y;
    (void)r;
    (void)n;
    (void)m;
    return 0;
}
int heic_simd_ycc_444_row(const uint16_t *a, const uint16_t *b, const uint16_t *c,
                          uint8_t *d, int w, int full, const int32_t *e, const int32_t *f,
                          const int32_t *g, const int32_t *h, const int32_t *i)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)w;
    (void)full;
    (void)e;
    (void)f;
    (void)g;
    (void)h;
    (void)i;
    return 0;
}
int heic_simd_ycc_420_row(const uint16_t *a, const uint16_t *b, const uint16_t *c,
                          uint8_t *d, int w, int p, int full, const int32_t *e,
                          const int32_t *f, const int32_t *g, const int32_t *h,
                          const int32_t *i)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)w;
    (void)p;
    (void)full;
    (void)e;
    (void)f;
    (void)g;
    (void)h;
    (void)i;
    return 0;
}
int heic_simd_chroma_edge4(uint16_t *p, int s, size_t b, int a, int t, int m, int al)
{
    (void)p;
    (void)s;
    (void)b;
    (void)a;
    (void)t;
    (void)m;
    (void)al;
    return 0;
}
int heic_simd_luma_filter4(uint16_t *p, size_t bp, size_t bq, size_t sa, size_t sc, int st,
                           int dep, int deq, int tc, int mv)
{
    (void)p;
    (void)bp;
    (void)bq;
    (void)sa;
    (void)sc;
    (void)st;
    (void)dep;
    (void)deq;
    (void)tc;
    (void)mv;
    return 0;
}
int heic_simd_dequant(int16_t *c, int n, int32_t comb, int sh)
{
    (void)c;
    (void)n;
    (void)comb;
    (void)sh;
    return 0;
}
int heic_simd_intra_ang_row(uint16_t *d, const int32_t *r, int n, int a, int b, int m)
{
    (void)d;
    (void)r;
    (void)n;
    (void)a;
    (void)b;
    (void)m;
    return 0;
}
int heic_simd_u16_to_i32_avail(const uint16_t *s, int32_t *b, int *a, int n)
{
    (void)s;
    (void)b;
    (void)a;
    (void)n;
    return 0;
}
int heic_simd_border_top_ext(const uint16_t *s, int32_t *b, int *a, int n)
{
    (void)s;
    (void)b;
    (void)a;
    (void)n;
    return 0;
}
int heic_simd_intra_ang_row_var(uint16_t *d, const int32_t *r, int n, int rb, int32_t ang,
                                int m)
{
    (void)d;
    (void)r;
    (void)n;
    (void)rb;
    (void)ang;
    (void)m;
    return 0;
}
int heic_simd_sao_band_row(uint16_t *r, int a, int b, int s, const int16_t *t, int m)
{
    (void)r;
    (void)a;
    (void)b;
    (void)s;
    (void)t;
    (void)m;
    return 0;
}
int heic_simd_sao_edge_h_row(const uint16_t *s, uint16_t *d, int a, int b, const int *o,
                             int m)
{
    (void)s;
    (void)d;
    (void)a;
    (void)b;
    (void)o;
    (void)m;
    return 0;
}
int heic_simd_sao_edge_v_row(const uint16_t *s, uint16_t *d, int st, int y, int a, int b,
                             const int *o, int m)
{
    (void)s;
    (void)d;
    (void)st;
    (void)y;
    (void)a;
    (void)b;
    (void)o;
    (void)m;
    return 0;
}

#endif /* HEIC_X86 */
