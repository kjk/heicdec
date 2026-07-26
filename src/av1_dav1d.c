/* av1_dav1d.c -- AV1 still-image decode via videolan dav1d (C).
 *
 * Build with -DHEIC_HAVE_DAV1D and link libdav1d (see cmd/build.ts -dav1d).
 *
 * Caches one Dav1dContext on heic_ctx so repeated stills (grids, multi-item,
 * profile loops) avoid dav1d_open/close. dav1d_flush after each still so the
 * next OBU sequence can start cleanly. Prefer feeding av1C configOBUs and
 * sample data as two wrap calls (no combined malloc) when both are present.
 */
#include "heic_internal.h"

#ifdef HEIC_HAVE_DAV1D
#include <dav1d/dav1d.h>
#include <string.h>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define HEIC_AV1_X86 1
#include <emmintrin.h>
#include <smmintrin.h>
#else
#define HEIC_AV1_X86 0
#endif

void heic_dav1d_ctx_close(heic_ctx *ctx)
{
    if (!ctx || !ctx->dav1d_ctx) return;
    dav1d_close((Dav1dContext **)&ctx->dav1d_ctx);
    ctx->dav1d_ctx = NULL;
}

static void dav1d_data_free_nop(const uint8_t *buf, void *cookie)
{
    (void)buf;
    (void)cookie;
}

/* Widen 8-bit → u16 plane copy (hot for AVIF stills). */
static void plane8_to_u16(uint16_t *dst, const uint8_t *src, int w, int h,
                          int dst_stride, int src_stride)
{
    int y, x;
    for (y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * (size_t)src_stride;
        uint16_t *d = dst + (size_t)y * (size_t)dst_stride;
        x = 0;
#if HEIC_AV1_X86
        /* 16 bytes → 16 u16 per iter (SSE4.1 cvtepu8). */
        for (; x + 16 <= w; x += 16) {
            __m128i b = _mm_loadu_si128((const __m128i *)(s + x));
            __m128i lo = _mm_cvtepu8_epi16(b);
            __m128i hi = _mm_cvtepu8_epi16(_mm_srli_si128(b, 8));
            _mm_storeu_si128((__m128i *)(d + x), lo);
            _mm_storeu_si128((__m128i *)(d + x + 8), hi);
        }
        for (; x + 8 <= w; x += 8) {
            __m128i b = _mm_loadl_epi64((const __m128i *)(s + x));
            _mm_storeu_si128((__m128i *)(d + x), _mm_cvtepu8_epi16(b));
        }
#endif
        for (; x < w; x++) d[x] = s[x];
    }
}

static int picture_to_frame(heic_ctx *ctx, const Dav1dPicture *pic, heic_frame *out)
{
    int w = pic->p.w;
    int h = pic->p.h;
    int bd = pic->p.bpc;
    int chroma;
    const uint8_t *ys, *us, *vs;
    int y_stride, u_stride, v_stride;

    if (w <= 0 || h <= 0) return -1;
    switch (pic->p.layout) {
    case DAV1D_PIXEL_LAYOUT_I400: chroma = 0; break;
    case DAV1D_PIXEL_LAYOUT_I420: chroma = 1; break;
    case DAV1D_PIXEL_LAYOUT_I422: chroma = 2; break;
    case DAV1D_PIXEL_LAYOUT_I444: chroma = 3; break;
    default: chroma = 1; break;
    }
    if (heic_frame_alloc(ctx, out, w, h, bd, chroma) != 0) return -1;

    out->full_range = pic->seq_hdr && pic->seq_hdr->color_range;
    if (pic->seq_hdr) {
        out->matrix_coeffs = (uint8_t)pic->seq_hdr->mtrx;
        out->color_primaries = (uint8_t)pic->seq_hdr->pri;
        out->transfer_characteristics = (uint8_t)pic->seq_hdr->trc;
    }

    y_stride = (int)pic->stride[0];
    u_stride = (int)pic->stride[1];
    v_stride = (int)pic->stride[1];
    ys = (const uint8_t *)pic->data[0];
    us = (const uint8_t *)pic->data[1];
    vs = (const uint8_t *)pic->data[2];

    if (bd == 8) {
        plane8_to_u16(out->y, ys, w, h, out->y_stride, y_stride);
        if (chroma > 0 && us && vs && out->cb && out->cr) {
            int cw = out->c_width, ch = out->c_height;
            plane8_to_u16(out->cb, us, cw, ch, out->c_stride, u_stride);
            plane8_to_u16(out->cr, vs, cw, ch, out->c_stride, v_stride);
        }
    } else {
        int y;
        /* 10/12-bit: little-endian u16 samples in dav1d planes */
        for (y = 0; y < h; y++) {
            const uint16_t *src =
                (const uint16_t *)(ys + (size_t)y * (size_t)y_stride);
            uint16_t *dst = out->y + (size_t)y * (size_t)out->y_stride;
            memcpy(dst, src, (size_t)w * sizeof(uint16_t));
        }
        if (chroma > 0 && us && vs && out->cb && out->cr) {
            int cw = out->c_width, ch = out->c_height;
            for (y = 0; y < ch; y++) {
                const uint16_t *su =
                    (const uint16_t *)(us + (size_t)y * (size_t)u_stride);
                const uint16_t *sv =
                    (const uint16_t *)(vs + (size_t)y * (size_t)v_stride);
                uint16_t *du = out->cb + (size_t)y * (size_t)out->c_stride;
                uint16_t *dv = out->cr + (size_t)y * (size_t)out->c_stride;
                memcpy(du, su, (size_t)cw * sizeof(uint16_t));
                memcpy(dv, sv, (size_t)cw * sizeof(uint16_t));
            }
        }
    }
    return 0;
}

static Dav1dContext *ensure_dav1d(heic_ctx *ctx)
{
    Dav1dSettings settings;
    Dav1dContext *c;

    if (ctx->dav1d_ctx) return (Dav1dContext *)ctx->dav1d_ctx;

    dav1d_default_settings(&settings);
    /* Still-image path: single thread matches our HEVC/bench fairness. */
    settings.n_threads = 1;
    settings.apply_grain = 0;
    settings.max_frame_delay = 1;
    /* Mute dav1d's stderr logger (e.g. "Error parsing OBU data" on bad bits). */
    settings.logger.callback = NULL;
    if (ctx->limits.max_pixels)
        settings.frame_size_limit = (unsigned)ctx->limits.max_pixels;

    c = NULL;
    if (dav1d_open(&c, &settings) != 0) return NULL;
    ctx->dav1d_ctx = c;
    return c;
}

struct heic_av1_sequence_state {
    heic_ctx *ctx;
    Dav1dContext *decoder;
    int config_sent;
};

heic_av1_sequence_state *heic_av1_sequence_new(heic_ctx *ctx)
{
    heic_av1_sequence_state *state;
    Dav1dSettings settings;
    if (!ctx) return NULL;
    state = (heic_av1_sequence_state *)heic_zalloc(ctx, sizeof(*state));
    if (!state) return NULL;
    state->ctx = ctx;
    dav1d_default_settings(&settings);
    settings.n_threads = 1;
    settings.apply_grain = 0;
    settings.max_frame_delay = 1;
    settings.logger.callback = NULL;
    if (ctx->limits.max_pixels)
        settings.frame_size_limit = (unsigned)ctx->limits.max_pixels;
    if (dav1d_open(&state->decoder, &settings) != 0) {
        heic_free_buf(ctx, state);
        heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_open failed");
        return NULL;
    }
    return state;
}

void heic_av1_sequence_destroy(heic_av1_sequence_state *state)
{
    heic_ctx *ctx;
    if (!state) return;
    ctx = state->ctx;
    if (state->decoder) dav1d_close(&state->decoder);
    heic_free_buf(ctx, state);
}

static int sequence_send_config(heic_av1_sequence_state *state,
                                const heic_av1c *cfg)
{
    Dav1dData data;
    int spins = 0;
    if (state->config_sent || !cfg || !cfg->config_obus_len) {
        state->config_sent = 1;
        return 0;
    }
    memset(&data, 0, sizeof(data));
    if (dav1d_data_wrap(&data, cfg->config_obus, cfg->config_obus_len,
                        dav1d_data_free_nop, NULL) != 0)
        return -1;
    while (data.sz && spins++ < 32) {
        int res = dav1d_send_data(state->decoder, &data);
        if (res == 0) continue;
        if (res == DAV1D_ERR(EAGAIN)) {
            Dav1dPicture pic;
            memset(&pic, 0, sizeof(pic));
            res = dav1d_get_picture(state->decoder, &pic);
            if (res == 0) {
                dav1d_picture_unref(&pic);
                continue;
            }
        }
        if (data.sz) dav1d_data_unref(&data);
        return -1;
    }
    if (data.sz) {
        dav1d_data_unref(&data);
        return -1;
    }
    state->config_sent = 1;
    return 0;
}

int heic_av1_sequence_decode(heic_av1_sequence_state *state,
                             const heic_av1c *cfg,
                             const uint8_t *data, size_t len,
                             heic_frame *out, const heic_abort *ab)
{
    Dav1dData input;
    int spins = 0, rc = -1;
    if (!state || !state->decoder || !data || !len || !out) return -1;
    memset(out, 0, sizeof(*out));
    memset(&input, 0, sizeof(input));
    if (heic_abort_check(ab)
        || sequence_send_config(state, cfg) != 0
        || dav1d_data_wrap(&input, data, len,
                           dav1d_data_free_nop, NULL) != 0)
        return -1;
    while (spins++ < 64) {
        int res;
        Dav1dPicture pic;
        if (heic_abort_check(ab)) break;
        if (input.sz) {
            res = dav1d_send_data(state->decoder, &input);
            if (res != 0 && res != DAV1D_ERR(EAGAIN)) break;
        }
        memset(&pic, 0, sizeof(pic));
        res = dav1d_get_picture(state->decoder, &pic);
        if (res == 0) {
            rc = picture_to_frame(state->ctx, &pic, out);
            dav1d_picture_unref(&pic);
            break;
        }
        if (res != DAV1D_ERR(EAGAIN) || !input.sz) break;
    }
    if (input.sz) dav1d_data_unref(&input);
    if (rc != 0)
        heic_error(state->ctx, HEIC_SEVERITY_ERROR,
                   "dav1d produced no sequence picture");
    return rc;
}

/* Push all of *pd into the decoder (handles EAGAIN by draining nothing —
 * stills are small). Returns 0 ok, <0 fatal, and leaves *pd.sz==0 when fully
 * consumed. */
static int send_all(Dav1dContext *c, Dav1dData *pd)
{
    while (pd->sz > 0) {
        int res = dav1d_send_data(c, pd);
        if (res == 0) continue;
        if (res == DAV1D_ERR(EAGAIN)) {
            /* Decoder full; try to pull a picture later. */
            return 0;
        }
        return res;
    }
    return 0;
}

int heic_av1_decode(heic_ctx *ctx, const heic_av1c *cfg,
                    const uint8_t *data, size_t len,
                    heic_frame *out, const heic_abort *ab)
{
    Dav1dContext *c = NULL;
    Dav1dData d_cfg, d_sample;
    Dav1dPicture pic;
    int rc = -1;
    int got = 0;
    int have_cfg = 0;

    memset(out, 0, sizeof(*out));
    memset(&d_cfg, 0, sizeof(d_cfg));
    memset(&d_sample, 0, sizeof(d_sample));
    if (!ctx || !data || len == 0) return -1;
    if (heic_abort_check(ab)) return -1;

    c = ensure_dav1d(ctx);
    if (!c) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_open failed");
        return -1;
    }

    /* Feed av1C configOBUs first (no combined malloc), then sample OBUs. */
    if (cfg && cfg->config_obus && cfg->config_obus_len > 0) {
        if (dav1d_data_wrap(&d_cfg, cfg->config_obus, cfg->config_obus_len,
                            dav1d_data_free_nop, NULL) != 0) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_data_wrap(config) failed");
            return -1;
        }
        have_cfg = 1;
        if (send_all(c, &d_cfg) < 0) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_send_data(config) failed");
            if (d_cfg.sz) dav1d_data_unref(&d_cfg);
            heic_dav1d_ctx_close(ctx);
            return -1;
        }
    }

    if (dav1d_data_wrap(&d_sample, data, len, dav1d_data_free_nop, NULL) != 0) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_data_wrap(sample) failed");
        if (have_cfg && d_cfg.sz) dav1d_data_unref(&d_cfg);
        return -1;
    }

    memset(&pic, 0, sizeof(pic));
    for (;;) {
        int res;
        if (heic_abort_check(ab)) break;

        if (have_cfg && d_cfg.sz > 0) {
            if (send_all(c, &d_cfg) < 0) {
                heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_send_data(config) failed");
                if (d_cfg.sz) dav1d_data_unref(&d_cfg);
                if (d_sample.sz) dav1d_data_unref(&d_sample);
                heic_dav1d_ctx_close(ctx);
                return -1;
            }
        }
        if (d_sample.sz > 0) {
            res = send_all(c, &d_sample);
            if (res < 0) {
                heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_send_data failed (%d)", res);
                if (d_sample.sz) dav1d_data_unref(&d_sample);
                if (have_cfg && d_cfg.sz) dav1d_data_unref(&d_cfg);
                heic_dav1d_ctx_close(ctx);
                return -1;
            }
        }

        res = dav1d_get_picture(c, &pic);
        if (res == 0) {
            rc = picture_to_frame(ctx, &pic, out);
            dav1d_picture_unref(&pic);
            got = 1;
            break;
        }
        if (res != DAV1D_ERR(EAGAIN)) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d_get_picture failed (%d)", res);
            heic_dav1d_ctx_close(ctx);
            c = NULL;
            break;
        }
        /* Need more input or drain */
        if (d_sample.sz == 0 && (!have_cfg || d_cfg.sz == 0)) {
            res = dav1d_get_picture(c, &pic);
            if (res == 0) {
                rc = picture_to_frame(ctx, &pic, out);
                dav1d_picture_unref(&pic);
                got = 1;
            }
            break;
        }
    }
    if (d_sample.sz) dav1d_data_unref(&d_sample);
    if (have_cfg && d_cfg.sz) dav1d_data_unref(&d_cfg);

    if (!got) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "dav1d produced no picture");
        rc = -1;
    }

    /* Clear delayed state so the next still (possibly new sequence) can reuse c. */
    if (c) dav1d_flush(c);
    return rc;
}

#else /* !HEIC_HAVE_DAV1D */

void heic_dav1d_ctx_close(heic_ctx *ctx)
{
    if (ctx) ctx->dav1d_ctx = NULL;
}

int heic_av1_decode(heic_ctx *ctx, const heic_av1c *cfg,
                    const uint8_t *data, size_t len,
                    heic_frame *out, const heic_abort *ab)
{
    (void)cfg;
    (void)data;
    (void)len;
    (void)ab;
    memset(out, 0, sizeof(*out));
    if (ctx)
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "AV1 decode requires dav1d (build with HEIC_HAVE_DAV1D)");
    return -1;
}

struct heic_av1_sequence_state {
    heic_ctx *ctx;
};

heic_av1_sequence_state *heic_av1_sequence_new(heic_ctx *ctx)
{
    if (ctx)
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "AV1 decode requires dav1d (build with HEIC_HAVE_DAV1D)");
    return NULL;
}

void heic_av1_sequence_destroy(heic_av1_sequence_state *state)
{
    (void)state;
}

int heic_av1_sequence_decode(heic_av1_sequence_state *state,
                             const heic_av1c *cfg,
                             const uint8_t *data, size_t len,
                             heic_frame *out, const heic_abort *ab)
{
    (void)state;
    (void)cfg;
    (void)data;
    (void)len;
    (void)ab;
    if (out) memset(out, 0, sizeof(*out));
    return -1;
}

#endif /* HEIC_HAVE_DAV1D */
