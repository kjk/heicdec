/* fuzz_common.h -- shared by the fuzz targets (test/fuzz_*.c).
 *
 * A budgeted allocator and a context with limits tight enough that one
 * pathological input cannot stall the fuzzer. Each target includes this once.
 */
#ifndef HEIC_FUZZ_COMMON_H
#define HEIC_FUZZ_COMMON_H

#include "heic.h"

#include <stdlib.h>
#include <string.h>

/* Budgeted allocator: crafted ispe/grid/SPS can declare huge canvases; bound
 * live bytes so those inputs fail cleanly via the library's alloc-failure
 * paths (fuzzing those too) instead of tripping the fuzzer's RSS limit. Each
 * block stores its size in a 16-byte header (keeps malloc alignment).
 * Covers the library's own allocations only: dav1d, zlib and brotli use the
 * C allocator directly. */
#define FUZZ_MEM_BUDGET ((size_t)256 << 20) /* 256 MB live per input */

/* Images larger than this are not decoded by the fuzz targets: decode time
 * grows with the pixel count, coverage does not. */
#define FUZZ_MAX_PIXELS ((uint64_t)2 << 20) /* 2 Mpx */

static size_t fuzz_live;

static void *fuzz_alloc(void *user, void *ctx, size_t size)
{
    uint8_t *p;
    (void)user;
    (void)ctx;
    if (size > FUZZ_MEM_BUDGET - fuzz_live) return NULL;
    p = (uint8_t *)malloc(size + 16);
    if (!p) return NULL;
    memcpy(p, &size, sizeof(size));
    fuzz_live += size;
    return p + 16;
}

static void fuzz_free(void *user, void *ctx, void *ptr)
{
    uint8_t *p;
    size_t size;
    (void)user;
    (void)ctx;
    if (!ptr) return;
    p = (uint8_t *)ptr - 16;
    memcpy(&size, p, sizeof(size));
    fuzz_live -= size;
    free(p);
}

/* A context on the budgeted allocator, limited to pictures of at most
   max_pixels. */
static inline heic_ctx *fuzz_ctx_new_limited(uint64_t max_pixels)
{
    heic_ctx *ctx;
    heic_limits lim;
    heic_init();
    fuzz_live = 0;
    ctx = heic_ctx_new(fuzz_alloc, fuzz_free, NULL, NULL);
    if (!ctx) return NULL;
    lim.max_width = 4096;
    lim.max_height = 4096;
    lim.max_pixels = max_pixels;
    lim.max_memory_bytes = FUZZ_MEM_BUDGET;
    heic_ctx_set_limits(ctx, &lim);
    return ctx;
}

/* The same with the default fuzzing limit (FUZZ_MAX_PIXELS). */
static inline heic_ctx *fuzz_ctx_new(void)
{
    return fuzz_ctx_new_limited(FUZZ_MAX_PIXELS);
}

/* Free the context; every allocation must be back by now. A non-zero balance
 * is a leak in the library (ASan has no leak detection on Windows, and the
 * other sanitizers have none at all, so check it here). */
static inline void fuzz_ctx_free(heic_ctx *ctx)
{
    heic_ctx_free(ctx);
    if (fuzz_live != 0) abort();
}

/* MemorySanitizer only reports uninitialized memory once the program
 * branches on it, and the targets never look at the pixels they get back.
 * So under MSan every decoded sample is checked explicitly; elsewhere these
 * do nothing. */
#if defined(__has_feature)
#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define FUZZ_CHECK_INIT(p, n) __msan_check_mem_is_initialized((p), (n))
#endif
#endif
#ifndef FUZZ_CHECK_INIT
#define FUZZ_CHECK_INIT(p, n) ((void)(p), (void)(n))
#endif

/* Every pixel of a decoded image (not the row padding) must be written. */
static inline void fuzz_check_pixels(const uint8_t *data, uint32_t width,
                                     uint32_t height, int stride,
                                     heic_format format)
{
    size_t row = (size_t)width
                 * (format == HEIC_FORMAT_RGBA || format == HEIC_FORMAT_BGRA
                        ? 4u : 3u);
    uint32_t y;
    if (!data) return;
    for (y = 0; y < height; y++)
        FUZZ_CHECK_INIT(data + (size_t)y * (size_t)stride, row);
}

static inline void fuzz_check_image(const heic_image *img)
{
    if (img)
        fuzz_check_pixels(img->data, img->width, img->height, img->stride,
                          img->format);
}

#endif
