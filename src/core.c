/* core.c -- context, alloc, diagnostics, version */
#include "heic_internal.h"

static void *default_alloc(void *user, void *ctx, size_t size)
{
    (void)user;
    (void)ctx;
    return malloc(size);
}

static void default_free(void *user, void *ctx, void *p)
{
    (void)user;
    (void)ctx;
    free(p);
}

static int g_inited;

void heic_init(void)
{
    g_inited = 1;
    heic_simd_init();
}

const char *heic_version(void)
{
    return "0.1.0-dev";
}

void heic_abort_init(heic_abort *ab)
{
    if (ab) ab->requested = 0;
}

void heic_abort_request(heic_abort *ab)
{
    if (ab) ab->requested = 1;
}

int heic_abort_check(const heic_abort *ab)
{
    return ab && ab->requested;
}

heic_ctx *heic_ctx_new(heic_alloc_cb alloc, heic_free_cb free_cb,
                       heic_error_cb error, void *user)
{
    heic_alloc_cb a = alloc ? alloc : default_alloc;
    heic_free_cb f = free_cb ? free_cb : default_free;
    heic_ctx *ctx = (heic_ctx *)a(user, NULL, sizeof(heic_ctx));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->alloc = a;
    ctx->free_cb = f;
    ctx->error = error;
    ctx->user = user;
    ctx->limits.max_width = HEIC_DEFAULT_MAX_WIDTH;
    ctx->limits.max_height = HEIC_DEFAULT_MAX_HEIGHT;
    ctx->limits.max_pixels = HEIC_DEFAULT_MAX_PIXELS;
    ctx->limits.max_memory_bytes = HEIC_DEFAULT_MAX_MEMORY;
    return ctx;
}

void heic_ctx_free(heic_ctx *ctx)
{
    if (!ctx) return;
    heic_dav1d_ctx_close(ctx);
    {
        heic_free_cb f = ctx->free_cb;
        void *user = ctx->user;
        f(user, NULL, ctx);
    }
}

void heic_ctx_set_limits(heic_ctx *ctx, const heic_limits *limits)
{
    if (!ctx || !limits) return;
    if (limits->max_width) ctx->limits.max_width = limits->max_width;
    if (limits->max_height) ctx->limits.max_height = limits->max_height;
    if (limits->max_pixels) ctx->limits.max_pixels = limits->max_pixels;
    if (limits->max_memory_bytes) ctx->limits.max_memory_bytes = limits->max_memory_bytes;
}

void *heic_zalloc(heic_ctx *ctx, size_t size)
{
    void *p;
    if (!ctx || size == 0) return NULL;
    p = ctx->alloc(ctx->user, ctx, size);
    if (p) memset(p, 0, size);
    return p;
}

void *heic_realloc_buf(heic_ctx *ctx, void *p, size_t old_size, size_t new_size)
{
    void *q;
    if (!ctx) return NULL;
    if (new_size == 0) {
        heic_free_buf(ctx, p);
        return NULL;
    }
    q = ctx->alloc(ctx->user, ctx, new_size);
    if (!q) return NULL;
    if (p) {
        size_t n = old_size < new_size ? old_size : new_size;
        memcpy(q, p, n);
        if (new_size > old_size) memset((uint8_t *)q + old_size, 0, new_size - old_size);
        heic_free_buf(ctx, p);
    } else {
        memset(q, 0, new_size);
    }
    return q;
}

void heic_free_buf(heic_ctx *ctx, void *p)
{
    if (!ctx || !p) return;
    ctx->free_cb(ctx->user, ctx, p);
}

void heic_free(heic_ctx *ctx, void *p)
{
    heic_free_buf(ctx, p);
}

void heic_error(heic_ctx *ctx, heic_severity sev, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!ctx || !ctx->error || !fmt) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    ctx->error(ctx->user, sev, buf);
}

void heic_image_destroy(heic_ctx *ctx, heic_image *img)
{
    if (!img) return;
    if (ctx) heic_free_buf(ctx, img->data);
    else free(img->data);
    if (ctx) heic_free_buf(ctx, img);
    else free(img);
}

void heic_frame_free(heic_ctx *ctx, heic_frame *f)
{
    if (!f) return;
    heic_free_buf(ctx, f->y);
    heic_free_buf(ctx, f->cb);
    heic_free_buf(ctx, f->cr);
    heic_free_buf(ctx, f->a);
    heic_free_buf(ctx, f->motion);
    heic_free_buf(ctx, f->motion_pred_mode);
    memset(f, 0, sizeof(*f));
}

int heic_frame_alloc(heic_ctx *ctx, heic_frame *f, int w, int h,
                     int bit_depth, int chroma_format)
{
    int cw, ch;
    size_t y_n, c_n;
    if (!ctx || !f || w <= 0 || h <= 0) return -1;
    if (w > (int)ctx->limits.max_width || h > (int)ctx->limits.max_height) return -1;
    if ((uint64_t)w * (uint64_t)h > ctx->limits.max_pixels) return -1;

    memset(f, 0, sizeof(*f));
    f->width = w;
    f->height = h;
    f->bit_depth = bit_depth;
    f->chroma_bit_depth = chroma_format ? bit_depth : 0;
    f->chroma_format = chroma_format;
    f->y_stride = w;

    switch (chroma_format) {
    case 0: cw = 0; ch = 0; break;
    case 1: cw = (w + 1) / 2; ch = (h + 1) / 2; break;
    case 2: cw = (w + 1) / 2; ch = h; break;
    case 3: cw = w; ch = h; break;
    default: return -1;
    }
    f->c_width = cw;
    f->c_height = ch;
    f->c_stride = cw > 0 ? cw : 0;

    y_n = (size_t)w * (size_t)h * sizeof(uint16_t);
    /* Allocate without zeroing — fill UNINIT (0xFFFF) via memset. */
    f->y = (uint16_t *)ctx->alloc(ctx->user, ctx, y_n);
    if (!f->y) return -1;
    /* HEIC_UNINIT_SAMPLE is 0xFFFF; one repstos is far cheaper than a scalar loop. */
    memset(f->y, 0xFF, y_n);
    if (cw > 0 && ch > 0) {
        c_n = (size_t)cw * (size_t)ch * sizeof(uint16_t);
        f->cb = (uint16_t *)ctx->alloc(ctx->user, ctx, c_n);
        f->cr = (uint16_t *)ctx->alloc(ctx->user, ctx, c_n);
        if (!f->cb || !f->cr) {
            heic_frame_free(ctx, f);
            return -1;
        }
        memset(f->cb, 0xFF, c_n);
        memset(f->cr, 0xFF, c_n);
    }
    return 0;
}
