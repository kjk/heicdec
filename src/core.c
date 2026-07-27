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
    heic_hevc_param_cache_free(ctx);
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

/* Size header before every tracked payload. Caller always sees payload. */
typedef struct {
    size_t size; /* payload bytes */
} heic_mem_hdr;

#define HEIC_MEM_HDR_SIZE sizeof(heic_mem_hdr)

static int heic_memory_would_exceed(const heic_ctx *ctx, size_t total)
{
    size_t cap;
    if (!ctx) return 1;
    cap = ctx->limits.max_memory_bytes;
    if (cap == 0) return 0; /* no limit (should not happen after ctx_new) */
    if (total > cap) return 1;
    if (ctx->live_bytes > cap - total) return 1;
    return 0;
}

static void *heic_alloc_raw(heic_ctx *ctx, size_t size, int zero)
{
    size_t total;
    uint8_t *raw;
    heic_mem_hdr *h;
    if (!ctx || size == 0) return NULL;
    if (size > SIZE_MAX - HEIC_MEM_HDR_SIZE) return NULL;
    total = size + HEIC_MEM_HDR_SIZE;
    if (heic_memory_would_exceed(ctx, total)) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "memory limit exceeded (need %zu, live %zu, cap %zu)",
                   total, ctx->live_bytes, ctx->limits.max_memory_bytes);
        return NULL;
    }
    raw = (uint8_t *)ctx->alloc(ctx->user, ctx, total);
    if (!raw) return NULL;
    h = (heic_mem_hdr *)raw;
    h->size = size;
    ctx->live_bytes += total;
    if (zero) memset(raw + HEIC_MEM_HDR_SIZE, 0, size);
    return raw + HEIC_MEM_HDR_SIZE;
}

void *heic_alloc(heic_ctx *ctx, size_t size)
{
    return heic_alloc_raw(ctx, size, 0);
}

void *heic_zalloc(heic_ctx *ctx, size_t size)
{
    return heic_alloc_raw(ctx, size, 1);
}

void *heic_realloc_buf(heic_ctx *ctx, void *p, size_t old_size, size_t new_size)
{
    void *q;
    if (!ctx) return NULL;
    if (new_size == 0) {
        heic_free_buf(ctx, p);
        return NULL;
    }
    q = heic_alloc_raw(ctx, new_size, 0);
    if (!q) return NULL;
    if (p) {
        size_t n = old_size < new_size ? old_size : new_size;
        memcpy(q, p, n);
        if (new_size > old_size)
            memset((uint8_t *)q + old_size, 0, new_size - old_size);
        heic_free_buf(ctx, p);
    } else {
        memset(q, 0, new_size);
    }
    return q;
}

void heic_free_buf(heic_ctx *ctx, void *p)
{
    heic_mem_hdr *h;
    size_t total;
    if (!ctx || !p) return;
    h = (heic_mem_hdr *)((uint8_t *)p - HEIC_MEM_HDR_SIZE);
    total = h->size + HEIC_MEM_HDR_SIZE;
    if (ctx->live_bytes >= total) ctx->live_bytes -= total;
    else ctx->live_bytes = 0;
    ctx->free_cb(ctx->user, ctx, h);
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

static void frame_chroma_dims(int w, int h, int chroma_format, int *cw, int *ch)
{
    switch (chroma_format) {
    case 0: *cw = 0; *ch = 0; break;
    case 1: *cw = (w + 1) / 2; *ch = (h + 1) / 2; break;
    case 2: *cw = (w + 1) / 2; *ch = h; break;
    case 3: *cw = w; *ch = h; break;
    default: *cw = -1; *ch = -1; break;
    }
}

int heic_frame_alloc(heic_ctx *ctx, heic_frame *f, int w, int h,
                     int bit_depth, int chroma_format)
{
    int cw, ch;
    size_t y_n, c_n;
    if (!ctx || !f || w <= 0 || h <= 0) return -1;
    if (w > (int)ctx->limits.max_width || h > (int)ctx->limits.max_height) return -1;
    if ((uint64_t)w * (uint64_t)h > ctx->limits.max_pixels) return -1;

    frame_chroma_dims(w, h, chroma_format, &cw, &ch);
    if (cw < 0) return -1;

    memset(f, 0, sizeof(*f));
    f->width = w;
    f->height = h;
    f->bit_depth = bit_depth;
    f->chroma_bit_depth = chroma_format ? bit_depth : 0;
    f->chroma_format = chroma_format;
    f->y_stride = w;
    f->c_width = cw;
    f->c_height = ch;
    f->c_stride = cw > 0 ? cw : 0;

    y_n = (size_t)w * (size_t)h * sizeof(uint16_t);
    /* Fill UNINIT (0xFFFF): some paths leave samples unread (crop, partial
     * CTBs, failed tiles); color conversion treats 0xFFFF specially. */
    f->y = (uint16_t *)heic_alloc(ctx, y_n);
    if (!f->y) return -1;
    memset(f->y, 0xFF, y_n);
    if (cw > 0 && ch > 0) {
        c_n = (size_t)cw * (size_t)ch * sizeof(uint16_t);
        f->cb = (uint16_t *)heic_alloc(ctx, c_n);
        f->cr = (uint16_t *)heic_alloc(ctx, c_n);
        if (!f->cb || !f->cr) {
            heic_frame_free(ctx, f);
            return -1;
        }
        memset(f->cb, 0xFF, c_n);
        memset(f->cr, 0xFF, c_n);
    }
    return 0;
}

/* Reuse existing plane buffers when geometry matches (grid tiles). */
int heic_frame_prepare(heic_ctx *ctx, heic_frame *f, int w, int h,
                       int bit_depth, int chroma_format)
{
    int cw, ch;
    size_t y_n, c_n;
    if (!ctx || !f || w <= 0 || h <= 0) return -1;
    if (w > (int)ctx->limits.max_width || h > (int)ctx->limits.max_height) return -1;
    if ((uint64_t)w * (uint64_t)h > ctx->limits.max_pixels) return -1;
    frame_chroma_dims(w, h, chroma_format, &cw, &ch);
    if (cw < 0) return -1;

    if (f->y && f->width == w && f->height == h && f->bit_depth == bit_depth
        && f->chroma_format == chroma_format && f->y_stride == w
        && f->c_width == cw && f->c_height == ch
        && (cw == 0 || (f->cb && f->cr && f->c_stride == cw))) {
        heic_free_buf(ctx, f->a);
        heic_free_buf(ctx, f->motion);
        heic_free_buf(ctx, f->motion_pred_mode);
        f->a = NULL;
        f->motion = NULL;
        f->motion_pred_mode = NULL;
        f->motion_n = 0;
        f->motion_stride = 0;
        f->motion_min_pu = 0;
        f->a_stride = 0;
        f->crop_left = f->crop_right = f->crop_top = f->crop_bottom = 0;
        f->poc = 0;
        f->poc_valid = 0;
        f->nal_unit_type = 0;
        f->temporal_id = 0;
        f->chroma_bit_depth = chroma_format ? bit_depth : 0;
        y_n = (size_t)w * (size_t)h * sizeof(uint16_t);
        memset(f->y, 0xFF, y_n);
        if (cw > 0) {
            c_n = (size_t)cw * (size_t)ch * sizeof(uint16_t);
            memset(f->cb, 0xFF, c_n);
            memset(f->cr, 0xFF, c_n);
        }
        return 0;
    }

    heic_frame_free(ctx, f);
    return heic_frame_alloc(ctx, f, w, h, bit_depth, chroma_format);
}
