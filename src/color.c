/* color.c -- YCbCr → interleaved 8-bit RGB (BT.601/709/2020, limited/full)
 *
 * Coefficients match imazen/heic (and libheif Op_YCbCr420_to_RGB24 full-range
 * ×256 fixed-point). 10/12-bit samples are right-shifted to 8-bit first.
 *
 * Hot path: matrix/range selected once per frame; 4:4:4 / 4:2:0 specialized
 * loops avoid per-pixel chroma-index math and function-call overhead.
 */
#include "heic_internal.h"

typedef struct {
    int full;
    int matrix; /* 0=GBR, 1=709, 9=2020, else 601 */
    /* full-range ×256 coeffs */
    int cr_r, cb_g, cr_g, cb_b;
    /* limited-range ×8192 path */
    int cr_r_l, cb_g_l, cr_g_l, cb_b_l;
} ycc_coeffs;

static void ycc_select(int matrix, int full, ycc_coeffs *c)
{
    c->full = full;
    c->matrix = matrix;
    if (matrix == 1) {
        c->cr_r = 403;
        c->cb_g = -48;
        c->cr_g = -120;
        c->cb_b = 475;
        c->cr_r_l = 14744;
        c->cb_g_l = -1754;
        c->cr_g_l = -4383;
        c->cb_b_l = 17373;
    } else if (matrix == 9) {
        c->cr_r = 377;
        c->cb_g = -42;
        c->cr_g = -146;
        c->cb_b = 482;
        c->cr_r_l = 13806;
        c->cb_g_l = -1541;
        c->cr_g_l = -5349;
        c->cb_b_l = 17615;
    } else {
        /* BT.601 (incl. matrix 5/6) */
        c->cr_r = 359;
        c->cb_g = -88;
        c->cr_g = -183;
        c->cb_b = 454;
        c->cr_r_l = 13126;
        c->cb_g_l = -3222;
        c->cr_g_l = -6686;
        c->cb_b_l = 16591;
    }
}

/* Clamp to 0..255 without a helper call in the hot loop. */
static inline uint8_t ycc_clamp8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Per-frame 8-bit LUTs: replace per-pixel multiplies/clamps with table loads. */
typedef struct {
    int32_t yv[256];   /* limited: (Y'-16)*9576; full: unused (Y used raw) */
    int32_t cr_r[256]; /* unshifted chroma contribs */
    int32_t cb_g[256];
    int32_t cr_g[256];
    int32_t cb_b[256];
    int full;
} ycc_lut;

static void ycc_build_lut(const ycc_coeffs *c, ycc_lut *lut)
{
    int i;
    lut->full = c->full;
    if (c->full) {
        for (i = 0; i < 256; i++) {
            int cz = i - 128;
            lut->yv[i] = i;
            lut->cr_r[i] = c->cr_r * cz;
            lut->cb_g[i] = c->cb_g * cz;
            lut->cr_g[i] = c->cr_g * cz;
            lut->cb_b[i] = c->cb_b * cz;
        }
    } else {
        for (i = 0; i < 256; i++) {
            int Y = i < 16 ? 16 : (i > 235 ? 235 : i);
            int C = i < 16 ? 16 : (i > 240 ? 240 : i);
            int cz = C - 128;
            lut->yv[i] = (Y - 16) * 9576;
            lut->cr_r[i] = c->cr_r_l * cz;
            lut->cb_g[i] = c->cb_g_l * cz;
            lut->cr_g[i] = c->cr_g_l * cz;
            lut->cb_b[i] = c->cb_b_l * cz;
        }
    }
}

static inline void ycc_pixel_lut(const ycc_lut *lut, int y, int cb, int cr,
                                 uint8_t *r, uint8_t *g, uint8_t *b)
{
    int rr, gg, bb;
    y &= 255;
    cb &= 255;
    cr &= 255;
    if (lut->full) {
        rr = lut->yv[y] + ((lut->cr_r[cr] + 128) >> 8);
        gg = lut->yv[y] + ((lut->cb_g[cb] + lut->cr_g[cr] + 128) >> 8);
        bb = lut->yv[y] + ((lut->cb_b[cb] + 128) >> 8);
    } else {
        rr = (lut->yv[y] + lut->cr_r[cr] + 4096) >> 13;
        gg = (lut->yv[y] + lut->cb_g[cb] + lut->cr_g[cr] + 4096) >> 13;
        bb = (lut->yv[y] + lut->cb_b[cb] + 4096) >> 13;
    }
    *r = ycc_clamp8(rr);
    *g = ycc_clamp8(gg);
    *b = ycc_clamp8(bb);
}

static inline void ycc_pixel(const ycc_coeffs *c, int y, int cb, int cr,
                             uint8_t *r, uint8_t *g, uint8_t *b)
{
    int cbz, crz, rr, gg, bb;

    if (c->matrix == 0) {
        *r = ycc_clamp8(cr);
        *g = ycc_clamp8(y);
        *b = ycc_clamp8(cb);
        return;
    }

    if (!c->full) {
        int yv;
        y = y < 16 ? 16 : (y > 235 ? 235 : y);
        cb = cb < 16 ? 16 : (cb > 240 ? 240 : cb);
        cr = cr < 16 ? 16 : (cr > 240 ? 240 : cr);
        cbz = cb - 128;
        crz = cr - 128;
        yv = (y - 16) * 9576;
        rr = (yv + c->cr_r_l * crz + 4096) >> 13;
        gg = (yv + c->cb_g_l * cbz + c->cr_g_l * crz + 4096) >> 13;
        bb = (yv + c->cb_b_l * cbz + 4096) >> 13;
    } else {
        cbz = cb - 128;
        crz = cr - 128;
        rr = y + ((c->cr_r * crz + 128) >> 8);
        gg = y + ((c->cb_g * cbz + c->cr_g * crz + 128) >> 8);
        bb = y + ((c->cb_b * cbz + 128) >> 8);
    }
    *r = ycc_clamp8(rr);
    *g = ycc_clamp8(gg);
    *b = ycc_clamp8(bb);
}

static inline void ycc_store(uint8_t *row, int is_bgr, int has_a, uint8_t r,
                             uint8_t g, uint8_t b, uint8_t a)
{
    if (is_bgr) {
        row[0] = b;
        row[1] = g;
        row[2] = r;
    } else {
        row[0] = r;
        row[1] = g;
        row[2] = b;
    }
    if (has_a) row[3] = a;
}

/* Hot path: 8-bit 4:4:4 → packed RGB via LUTs (+ SIMD row when available). */
static void convert_444_8_rgb(const heic_frame *f, const ycc_lut *lut, int x0, int y0,
                              int w, int h, uint8_t *dst, int stride)
{
    int y, x;
    int full = lut->full;
    const int32_t *yv = lut->yv;
    const int32_t *cr_r = lut->cr_r;
    const int32_t *cb_g = lut->cb_g;
    const int32_t *cr_g = lut->cr_g;
    const int32_t *cb_b = lut->cb_b;
    for (y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * (size_t)stride;
        const uint16_t *yp =
            f->y + (size_t)(y0 + y) * (size_t)f->y_stride + (size_t)x0;
        const uint16_t *cbp =
            f->cb + (size_t)(y0 + y) * (size_t)f->c_stride + (size_t)x0;
        const uint16_t *crp =
            f->cr + (size_t)(y0 + y) * (size_t)f->c_stride + (size_t)x0;
        if (heic_simd_ycc_444_row(yp, cbp, crp, row, w, full, yv, cr_r, cb_g, cr_g,
                                  cb_b))
            continue;
        if (full) {
            for (x = 0; x < w; x++) {
                int Y = (int)yp[x] & 255;
                int Cb = (int)cbp[x] & 255;
                int Cr = (int)crp[x] & 255;
                int rr = yv[Y] + ((cr_r[Cr] + 128) >> 8);
                int gg = yv[Y] + ((cb_g[Cb] + cr_g[Cr] + 128) >> 8);
                int bb = yv[Y] + ((cb_b[Cb] + 128) >> 8);
                row[0] = ycc_clamp8(rr);
                row[1] = ycc_clamp8(gg);
                row[2] = ycc_clamp8(bb);
                row += 3;
            }
        } else {
            for (x = 0; x < w; x++) {
                int Y = (int)yp[x] & 255;
                int Cb = (int)cbp[x] & 255;
                int Cr = (int)crp[x] & 255;
                int rr = (yv[Y] + cr_r[Cr] + 4096) >> 13;
                int gg = (yv[Y] + cb_g[Cb] + cr_g[Cr] + 4096) >> 13;
                int bb = (yv[Y] + cb_b[Cb] + 4096) >> 13;
                row[0] = ycc_clamp8(rr);
                row[1] = ycc_clamp8(gg);
                row[2] = ycc_clamp8(bb);
                row += 3;
            }
        }
    }
}

/* General 4:4:4 (alpha / BGR / >8-bit / GBR). */
static void convert_444(const heic_frame *f, const ycc_coeffs *cc, int x0, int y0,
                        int w, int h, int shift, uint8_t *dst, int stride, int bpp,
                        int is_bgr, int has_a)
{
    int y, x;
    for (y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * (size_t)stride;
        int sy = y0 + y;
        const uint16_t *yp = f->y + (size_t)sy * (size_t)f->y_stride + (size_t)x0;
        const uint16_t *cbp = f->cb + (size_t)sy * (size_t)f->c_stride + (size_t)x0;
        const uint16_t *crp = f->cr + (size_t)sy * (size_t)f->c_stride + (size_t)x0;
        const uint16_t *ap = NULL;
        if (has_a && f->a)
            ap = f->a + (size_t)sy * (size_t)(f->a_stride ? f->a_stride : f->y_stride) +
                 (size_t)x0;
        for (x = 0; x < w; x++) {
            int Y = (int)(yp[x] >> shift);
            int Cb = (int)(cbp[x] >> shift);
            int Cr = (int)(crp[x] >> shift);
            uint8_t r, g, b, av = 255;
            ycc_pixel(cc, Y, Cb, Cr, &r, &g, &b);
            if (ap) av = (uint8_t)(ap[x] >> shift);
            ycc_store(row, is_bgr, has_a, r, g, b, av);
            row += bpp;
        }
    }
}

/* 8-bit 4:2:0 → packed RGB via LUTs; expand chroma to a temp 444 row for SIMD. */
static void convert_420_8_rgb(const heic_frame *f, const ycc_lut *lut, int x0, int y0,
                              int w, int h, uint8_t *dst, int stride)
{
    int y, x;
    uint16_t *cb_row = NULL, *cr_row = NULL;
    uint16_t *heap_chr = NULL;
    /* Always heap: a 16KB stack temp + HEVC decode frames trips ASan stack limits. */
    if (heic_simd_enabled() && w >= 4) {
        heap_chr = (uint16_t *)malloc((size_t)w * 2u * sizeof(uint16_t));
        if (heap_chr) {
            cb_row = heap_chr;
            cr_row = heap_chr + w;
        }
    }
    for (y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * (size_t)stride;
        int sy = y0 + y;
        int cy = sy >> 1;
        const uint16_t *yp = f->y + (size_t)sy * (size_t)f->y_stride + (size_t)x0;
        const uint16_t *cbp, *crp;
        if (cy >= f->c_height) cy = f->c_height - 1;
        if (cy < 0) cy = 0;
        cbp = f->cb + (size_t)cy * (size_t)f->c_stride;
        crp = f->cr + (size_t)cy * (size_t)f->c_stride;
        if (cb_row && cr_row) {
            /* Expand chroma 2× horizontally; pair samples share one Cb/Cr. */
            int cx0 = x0 >> 1;
            for (x = 0; x < w; ) {
                int cx = cx0 + (x >> 1);
                uint16_t cb, cr;
                if (cx >= f->c_width) cx = f->c_width - 1;
                if (cx < 0) cx = 0;
                cb = cbp[cx];
                cr = crp[cx];
                cb_row[x] = cb;
                cr_row[x] = cr;
                x++;
                if (x < w) {
                    cb_row[x] = cb;
                    cr_row[x] = cr;
                    x++;
                }
            }
            if (heic_simd_ycc_444_row(yp, cb_row, cr_row, row, w, lut->full, lut->yv,
                                      lut->cr_r, lut->cb_g, lut->cr_g, lut->cb_b))
                continue;
        }
        for (x = 0; x < w; x++) {
            int cx = (x0 + x) >> 1;
            uint8_t r, g, b;
            if (cx >= f->c_width) cx = f->c_width - 1;
            if (cx < 0) cx = 0;
            ycc_pixel_lut(lut, (int)yp[x], (int)cbp[cx], (int)crp[cx], &r, &g, &b);
            row[0] = r;
            row[1] = g;
            row[2] = b;
            row += 3;
        }
    }
    free(heap_chr);
}

static void convert_420(const heic_frame *f, const ycc_coeffs *cc, int x0, int y0,
                        int w, int h, int shift, uint8_t *dst, int stride, int bpp,
                        int is_bgr, int has_a)
{
    int y, x;
    for (y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * (size_t)stride;
        int sy = y0 + y;
        int cy = sy >> 1;
        const uint16_t *yp = f->y + (size_t)sy * (size_t)f->y_stride + (size_t)x0;
        const uint16_t *cbp, *crp;
        const uint16_t *ap = NULL;
        if (cy >= f->c_height) cy = f->c_height - 1;
        if (cy < 0) cy = 0;
        cbp = f->cb + (size_t)cy * (size_t)f->c_stride;
        crp = f->cr + (size_t)cy * (size_t)f->c_stride;
        if (has_a && f->a)
            ap = f->a + (size_t)sy * (size_t)(f->a_stride ? f->a_stride : f->y_stride) +
                 (size_t)x0;
        for (x = 0; x < w; x++) {
            int cx = (x0 + x) >> 1;
            int Y, Cb, Cr;
            uint8_t r, g, b, av = 255;
            if (cx >= f->c_width) cx = f->c_width - 1;
            if (cx < 0) cx = 0;
            Y = (int)(yp[x] >> shift);
            Cb = (int)(cbp[cx] >> shift);
            Cr = (int)(crp[cx] >> shift);
            ycc_pixel(cc, Y, Cb, Cr, &r, &g, &b);
            if (ap) av = (uint8_t)(ap[x] >> shift);
            ycc_store(row, is_bgr, has_a, r, g, b, av);
            row += bpp;
        }
    }
}

int heic_frame_to_rgb(heic_ctx *ctx, const heic_frame *f, heic_format format,
                      uint8_t *dst, int stride)
{
    int x0, y0, x1, y1, w, h;
    int shift;
    int full;
    int matrix;
    int bpp;
    int is_bgr;
    int has_a;
    ycc_coeffs cc;

    (void)ctx;
    if (!f || !f->y || !dst) return -1;
    x0 = f->crop_left;
    y0 = f->crop_top;
    x1 = f->width - f->crop_right;
    y1 = f->height - f->crop_bottom;
    if (x1 <= x0 || y1 <= y0) return -1;
    w = x1 - x0;
    h = y1 - y0;
    shift = f->bit_depth > 8 ? f->bit_depth - 8 : 0;
    full = f->full_range;
    matrix = f->matrix_coeffs;
    /* matrix 0 is GBR identity; only valid for 4:4:4. Otherwise fall back to
     * BT.601 (libheif sRGB default) rather than BT.709. */
    if (matrix == 0 && f->chroma_format != 3) matrix = 6;
    if (matrix == 2) matrix = 6; /* unspecified → BT.601 */

    bpp = (format == HEIC_FORMAT_RGBA || format == HEIC_FORMAT_BGRA) ? 4 : 3;
    is_bgr = (format == HEIC_FORMAT_BGR || format == HEIC_FORMAT_BGRA);
    has_a = (bpp == 4);
    if (stride < w * bpp) return -1;

    ycc_select(matrix, full, &cc);

    /* Fast 8-bit RGB (no alpha/BGR): one LUT build, then table-driven convert. */
    if (shift == 0 && !has_a && !is_bgr && matrix != 0
        && f->cb && f->cr
        && (f->chroma_format == 1 || f->chroma_format == 3)) {
        /* Heap: ycc_lut is ~5KB; keep it off the ASan stack. */
        ycc_lut *lut = (ycc_lut *)malloc(sizeof(ycc_lut));
        if (!lut) return -1;
        ycc_build_lut(&cc, lut);
        if (f->chroma_format == 3)
            convert_444_8_rgb(f, lut, x0, y0, w, h, dst, stride);
        else
            convert_420_8_rgb(f, lut, x0, y0, w, h, dst, stride);
        free(lut);
        return 0;
    }

    /* Mono / missing chroma */
    if (f->chroma_format == 0 || !f->cb || !f->cr) {
        int y, x;
        for (y = 0; y < h; y++) {
            uint8_t *row = dst + (size_t)y * (size_t)stride;
            int sy = y0 + y;
            const uint16_t *yp = f->y + (size_t)sy * (size_t)f->y_stride + (size_t)x0;
            const uint16_t *ap = NULL;
            if (has_a && f->a)
                ap = f->a +
                     (size_t)sy * (size_t)(f->a_stride ? f->a_stride : f->y_stride) +
                     (size_t)x0;
            for (x = 0; x < w; x++) {
                uint16_t ys = yp[x];
                int Y;
                uint8_t r, g, b, av = 255;
                if (ys == HEIC_UNINIT_SAMPLE) ys = 0;
                Y = (int)(ys >> shift);
                ycc_pixel(&cc, Y, 128, 128, &r, &g, &b);
                if (ap) {
                    uint16_t as = ap[x];
                    if (as != HEIC_UNINIT_SAMPLE) av = (uint8_t)(as >> shift);
                }
                ycc_store(row, is_bgr, has_a, r, g, b, av);
                row += bpp;
            }
        }
        return 0;
    }

    if (f->chroma_format == 3) {
        convert_444(f, &cc, x0, y0, w, h, shift, dst, stride, bpp, is_bgr, has_a);
        return 0;
    }
    if (f->chroma_format == 1) {
        convert_420(f, &cc, x0, y0, w, h, shift, dst, stride, bpp, is_bgr, has_a);
        return 0;
    }

    /* 4:2:2 (chroma_format == 2) and anything else: general path. */
    {
        int y, x;
        for (y = 0; y < h; y++) {
            uint8_t *row = dst + (size_t)y * (size_t)stride;
            int sy = y0 + y;
            for (x = 0; x < w; x++) {
                int sx = x0 + x;
                int Y, Cb, Cr;
                uint8_t r, g, b, av = 255;
                uint16_t ys = f->y[(size_t)sy * (size_t)f->y_stride + (size_t)sx];
                int cx = sx >> 1, cy = sy;
                uint16_t cbs, crs;
                if (ys == HEIC_UNINIT_SAMPLE) ys = 0;
                Y = (int)(ys >> shift);
                if (cx >= f->c_width) cx = f->c_width - 1;
                if (cy >= f->c_height) cy = f->c_height - 1;
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                cbs = f->cb[(size_t)cy * (size_t)f->c_stride + (size_t)cx];
                crs = f->cr[(size_t)cy * (size_t)f->c_stride + (size_t)cx];
                if (cbs == HEIC_UNINIT_SAMPLE) cbs = (uint16_t)(128u << shift);
                if (crs == HEIC_UNINIT_SAMPLE) crs = (uint16_t)(128u << shift);
                Cb = (int)(cbs >> shift);
                Cr = (int)(crs >> shift);
                ycc_pixel(&cc, Y, Cb, Cr, &r, &g, &b);
                if (has_a && f->a) {
                    uint16_t as =
                        f->a[(size_t)sy * (size_t)(f->a_stride ? f->a_stride : f->y_stride) +
                             (size_t)sx];
                    if (as != HEIC_UNINIT_SAMPLE) av = (uint8_t)(as >> shift);
                }
                ycc_store(row, is_bgr, has_a, r, g, b, av);
                row += bpp;
            }
        }
    }
    return 0;
}
