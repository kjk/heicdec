/* color.c -- YCbCr → interleaved 8-bit RGB (BT.601/709/2020, limited/full)
 *
 * Coefficients match imazen/heic (and libheif Op_YCbCr420_to_RGB24 full-range
 * ×256 fixed-point). Samples deeper than 8 bits are right-shifted to 8 first.
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
                        int w, int h, int y_shift, int c_shift, uint8_t *dst,
                        int stride, int bpp, int is_bgr, int has_a)
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
            int Y = (int)(yp[x] >> y_shift);
            int Cb = (int)(cbp[x] >> c_shift);
            int Cr = (int)(crp[x] >> c_shift);
            uint8_t r, g, b, av = 255;
            ycc_pixel(cc, Y, Cb, Cr, &r, &g, &b);
            if (ap) av = (uint8_t)(ap[x] >> y_shift);
            ycc_store(row, is_bgr, has_a, r, g, b, av);
            row += bpp;
        }
    }
}

/* 8-bit 4:2:0 → packed RGB via LUTs. */
static void convert_420_8_rgb(const heic_frame *f, const ycc_lut *lut, int x0, int y0,
                              int w, int h, uint8_t *dst, int stride)
{
    int y, x;
    int cx0 = x0 >> 1;
    for (y = 0; y < h; ) {
        uint8_t *row = dst + (size_t)y * (size_t)stride;
        int sy = y0 + y;
        int cy = sy >> 1;
        const uint16_t *yp = f->y + (size_t)sy * (size_t)f->y_stride + (size_t)x0;
        const uint16_t *cbp, *crp;
        if (cy >= f->c_height) cy = f->c_height - 1;
        if (cy < 0) cy = 0;
        cbp = f->cb + (size_t)cy * (size_t)f->c_stride;
        crp = f->cr + (size_t)cy * (size_t)f->c_stride;
        /* Even source row + next display row share the same chroma line. */
        if ((sy & 1) == 0 && y + 1 < h && cx0 >= 0 && cx0 < f->c_width) {
            const uint16_t *yp1 =
                f->y + (size_t)(sy + 1) * (size_t)f->y_stride + (size_t)x0;
            uint8_t *row1 = dst + (size_t)(y + 1) * (size_t)stride;
            if (heic_simd_ycc_420_2rows(yp, yp1, cbp + cx0, crp + cx0, row, row1, w,
                                        x0 & 1, lut->full, lut->yv, lut->cr_r,
                                        lut->cb_g, lut->cr_g, lut->cb_b)) {
                y += 2;
                continue;
            }
        }
        if (cx0 >= 0 && cx0 < f->c_width &&
            heic_simd_ycc_420_row(yp, cbp + cx0, crp + cx0, row, w, x0 & 1,
                                  lut->full, lut->yv, lut->cr_r, lut->cb_g,
                                  lut->cr_g, lut->cb_b)) {
            y++;
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
        y++;
    }
}

static void convert_420(const heic_frame *f, const ycc_coeffs *cc, int x0, int y0,
                        int w, int h, int y_shift, int c_shift, uint8_t *dst,
                        int stride, int bpp, int is_bgr, int has_a)
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
            Y = (int)(yp[x] >> y_shift);
            Cb = (int)(cbp[cx] >> c_shift);
            Cr = (int)(crp[cx] >> c_shift);
            ycc_pixel(cc, Y, Cb, Cr, &r, &g, &b);
            if (ap) av = (uint8_t)(ap[x] >> y_shift);
            ycc_store(row, is_bgr, has_a, r, g, b, av);
            row += bpp;
        }
    }
}

/* Reorder packed RGB rows into BGR / RGBA / BGRA, taking alpha from `ap`
   (>> a_shift) when present, else opaque. */
static void ycc_reorder_rows(const uint8_t *rgb, int n, int w, uint8_t *dst,
                             int stride, int bpp, int is_bgr,
                             const uint16_t *ap, int a_stride, int a_shift)
{
    int r, x;
    int ri = is_bgr ? 2 : 0, bi = is_bgr ? 0 : 2;
    for (r = 0; r < n; r++) {
        const uint8_t *s = rgb + (size_t)r * (size_t)w * 3;
        uint8_t *d = dst + (size_t)r * (size_t)stride;
        if (bpp == 3) {
            for (x = 0; x < w; x++) {
                d[ri] = s[0];
                d[1] = s[1];
                d[bi] = s[2];
                s += 3;
                d += 3;
            }
        } else if (ap) {
            const uint16_t *a = ap + (size_t)r * (size_t)a_stride;
            for (x = 0; x < w; x++) {
                d[ri] = s[0];
                d[1] = s[1];
                d[bi] = s[2];
                d[3] = (uint8_t)(a[x] >> a_shift);
                s += 3;
                d += 4;
            }
        } else {
            for (x = 0; x < w; x++) {
                d[ri] = s[0];
                d[1] = s[1];
                d[bi] = s[2];
                d[3] = 255;
                s += 3;
                d += 4;
            }
        }
    }
}

/* 4:2:0 / 4:4:4 through the 8-bit LUT/SIMD RGB kernels, a strip of rows at a
   time, for everything the direct call cannot do:
   - BGR / RGBA / BGRA: convert the strip to RGB, then reorder it;
   - more than 8 bits: first shift the strip's samples down to 8 bits into
     scratch planes laid out as a small frame with the same chroma phase.
   Returns -1 when a scratch buffer cannot be allocated (or the geometry is
   degenerate); the caller then uses the general per-pixel path. */
#define YCC_STRIP_ROWS 16
static int convert_via_rgb8(heic_ctx *ctx, const heic_frame *f,
                            const ycc_lut *lut, int x0, int y0, int w, int h,
                            int y_shift, int c_shift, uint8_t *dst, int stride,
                            int bpp, int is_bgr)
{
    int is420 = f->chroma_format == 1;
    int hbd = y_shift != 0 || c_shift != 0;
    int direct = bpp == 3 && !is_bgr; /* kernels write the caller's rows */
    int a_stride = f->a_stride ? f->a_stride : f->y_stride;
    /* Scratch frame (hbd only): luma column/row 0 keeps the parity of the
       source position so chroma siting is unchanged. */
    int px = is420 ? (x0 & 1) : 0;
    int vw = w + px;
    int cx0 = is420 ? (x0 >> 1) : x0;
    int cx1 = is420 ? ((x0 + w - 1) >> 1) : (x0 + w - 1);
    int vcw, vcs;
    heic_frame g;
    uint16_t *ybuf = NULL, *cbbuf = NULL, *crbuf = NULL;
    uint8_t *strip = NULL;
    int y, rc = -1;

    memset(&g, 0, sizeof(g));
    if (cx1 > f->c_width - 1) cx1 = f->c_width - 1;
    if (cx0 < 0 || cx1 < cx0) return -1;
    vcw = cx1 - cx0 + 1;
    vcs = vcw + 2; /* two replicated samples: the SIMD kernels read ahead */

    if (!direct) {
        strip = (uint8_t *)heic_alloc(ctx, (size_t)w * 3 * YCC_STRIP_ROWS);
        if (!strip) goto done;
    }
    if (hbd) {
        size_t rows = YCC_STRIP_ROWS + 1;
        ybuf = (uint16_t *)heic_alloc(ctx, (size_t)vw * rows * sizeof(uint16_t));
        cbbuf = (uint16_t *)heic_alloc(ctx, (size_t)vcs * rows * sizeof(uint16_t));
        crbuf = (uint16_t *)heic_alloc(ctx, (size_t)vcs * rows * sizeof(uint16_t));
        if (!ybuf || !cbbuf || !crbuf) goto done;
        /* Row 0 / column 0 are only read as padding when the parity is odd. */
        memset(ybuf, 0, (size_t)vw * rows * sizeof(uint16_t));
        g.chroma_format = f->chroma_format;
        g.width = vw;
        g.y = ybuf;
        g.y_stride = vw;
        g.cb = cbbuf;
        g.cr = crbuf;
        g.c_stride = vcs;
        g.c_width = vcw;
    }

    for (y = 0; y < h; y += YCC_STRIP_ROWS) {
        int n = h - y < YCC_STRIP_ROWS ? h - y : YCC_STRIP_ROWS;
        int sy0 = y0 + y;
        uint8_t *out_rows = dst + (size_t)y * (size_t)stride;
        uint8_t *tgt = direct ? out_rows : strip;
        int tstride = direct ? stride : w * 3;
        const heic_frame *src = f;
        int sx = x0, sy = sy0;

        if (hbd) {
            int py = is420 ? (sy0 & 1) : 0;
            int cy0 = is420 ? (sy0 >> 1) : sy0;
            int cy1 = is420 ? ((sy0 + n - 1) >> 1) : (sy0 + n - 1);
            int r, i;
            if (cy1 > f->c_height - 1) cy1 = f->c_height - 1;
            if (cy1 < cy0) goto done;
            for (r = 0; r < n; r++) {
                const uint16_t *s =
                    f->y + (size_t)(sy0 + r) * (size_t)f->y_stride + (size_t)x0;
                uint16_t *d = ybuf + (size_t)(py + r) * (size_t)vw + (size_t)px;
                for (i = 0; i < w; i++) d[i] = (uint16_t)(s[i] >> y_shift);
            }
            for (r = 0; r <= cy1 - cy0; r++) {
                size_t si = (size_t)(cy0 + r) * (size_t)f->c_stride + (size_t)cx0;
                uint16_t *db = cbbuf + (size_t)r * (size_t)vcs;
                uint16_t *dr = crbuf + (size_t)r * (size_t)vcs;
                for (i = 0; i < vcw; i++) {
                    db[i] = (uint16_t)(f->cb[si + (size_t)i] >> c_shift);
                    dr[i] = (uint16_t)(f->cr[si + (size_t)i] >> c_shift);
                }
                db[vcw] = db[vcw + 1] = db[vcw - 1];
                dr[vcw] = dr[vcw + 1] = dr[vcw - 1];
            }
            g.height = py + n;
            g.c_height = cy1 - cy0 + 1;
            src = &g;
            sx = px;
            sy = py;
        }

        if (is420)
            convert_420_8_rgb(src, lut, sx, sy, w, n, tgt, tstride);
        else
            convert_444_8_rgb(src, lut, sx, sy, w, n, tgt, tstride);

        if (!direct)
            ycc_reorder_rows(strip, n, w, out_rows, stride, bpp, is_bgr,
                             (bpp == 4 && f->a)
                                 ? f->a + (size_t)sy0 * (size_t)a_stride + (size_t)x0
                                 : NULL,
                             a_stride, y_shift);
    }
    rc = 0;
done:
    heic_free_buf(ctx, strip);
    heic_free_buf(ctx, ybuf);
    heic_free_buf(ctx, cbbuf);
    heic_free_buf(ctx, crbuf);
    return rc;
}

int heic_frame_to_rgb(heic_ctx *ctx, const heic_frame *f, heic_format format,
                      uint8_t *dst, int stride)
{
    int x0, y0, x1, y1, w, h;
    int y_shift, c_shift;
    int y_uninit, c_uninit;
    int full;
    int matrix;
    int bpp;
    int is_bgr;
    int has_a;
    ycc_coeffs cc;

    if (!f || !f->y || !dst) return -1;
    x0 = f->crop_left;
    y0 = f->crop_top;
    x1 = f->width - f->crop_right;
    y1 = f->height - f->crop_bottom;
    if (x1 <= x0 || y1 <= y0) return -1;
    w = x1 - x0;
    h = y1 - y0;
    y_shift = f->bit_depth > 8 ? f->bit_depth - 8 : 0;
    c_shift = f->chroma_bit_depth > 8 ? f->chroma_bit_depth - 8 : 0;
    /* 0xFFFF marks a sample nothing wrote, except at 16 bits, where it is
       an ordinary (full-scale) value. */
    y_uninit = f->bit_depth < 16;
    c_uninit = f->chroma_bit_depth < 16;
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

    /* Fast path: one LUT build, then table-driven / SIMD convert. 8-bit RGB
       is written directly; other channel orders and higher bit depths go
       through convert_via_rgb8. */
    if (matrix != 0 && f->cb && f->cr
        && (f->chroma_format == 1 || f->chroma_format == 3)) {
        ycc_lut lut;
        ycc_build_lut(&cc, &lut);
        if (y_shift == 0 && c_shift == 0 && !has_a && !is_bgr) {
            if (f->chroma_format == 3)
                convert_444_8_rgb(f, &lut, x0, y0, w, h, dst, stride);
            else
                convert_420_8_rgb(f, &lut, x0, y0, w, h, dst, stride);
            return 0;
        }
        if (convert_via_rgb8(ctx, f, &lut, x0, y0, w, h, y_shift, c_shift, dst,
                             stride, bpp, is_bgr) == 0)
            return 0;
    }

    /* Mono / missing chroma: R = G = B depends on luma only, so convert the
       256 possible 8-bit values once instead of running the matrix per
       pixel. */
    if (f->chroma_format == 0 || !f->cb || !f->cr) {
        int y, x;
        uint8_t gray[256];
        for (x = 0; x < 256; x++) {
            uint8_t r, g, b;
            ycc_pixel(&cc, x, 128, 128, &r, &g, &b);
            gray[x] = g;
        }
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
                if (ys == HEIC_UNINIT_SAMPLE && y_uninit) ys = 0;
                Y = (int)(ys >> y_shift);
                r = g = b = gray[Y > 255 ? 255 : Y];
                if (ap) {
                    uint16_t as = ap[x];
                    if (as != HEIC_UNINIT_SAMPLE || !y_uninit)
                        av = (uint8_t)(as >> y_shift);
                }
                ycc_store(row, is_bgr, has_a, r, g, b, av);
                row += bpp;
            }
        }
        return 0;
    }

    if (f->chroma_format == 3) {
        convert_444(f, &cc, x0, y0, w, h, y_shift, c_shift,
                    dst, stride, bpp, is_bgr, has_a);
        return 0;
    }
    if (f->chroma_format == 1) {
        convert_420(f, &cc, x0, y0, w, h, y_shift, c_shift,
                    dst, stride, bpp, is_bgr, has_a);
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
                if (ys == HEIC_UNINIT_SAMPLE && y_uninit) ys = 0;
                Y = (int)(ys >> y_shift);
                if (cx >= f->c_width) cx = f->c_width - 1;
                if (cy >= f->c_height) cy = f->c_height - 1;
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                cbs = f->cb[(size_t)cy * (size_t)f->c_stride + (size_t)cx];
                crs = f->cr[(size_t)cy * (size_t)f->c_stride + (size_t)cx];
                if (cbs == HEIC_UNINIT_SAMPLE && c_uninit)
                    cbs = (uint16_t)(128u << c_shift);
                if (crs == HEIC_UNINIT_SAMPLE && c_uninit)
                    crs = (uint16_t)(128u << c_shift);
                Cb = (int)(cbs >> c_shift);
                Cr = (int)(crs >> c_shift);
                ycc_pixel(&cc, Y, Cb, Cr, &r, &g, &b);
                if (has_a && f->a) {
                    uint16_t as =
                        f->a[(size_t)sy * (size_t)(f->a_stride ? f->a_stride : f->y_stride) +
                             (size_t)sx];
                    if (as != HEIC_UNINIT_SAMPLE || !y_uninit)
                        av = (uint8_t)(as >> y_shift);
                }
                ycc_store(row, is_bgr, has_a, r, g, b, av);
                row += bpp;
            }
        }
    }
    return 0;
}
