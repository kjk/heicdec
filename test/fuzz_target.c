/* fuzz_target.c -- libFuzzer / AFL++ entry point for the HEIC/HEIF/AVIF decoder.
 *
 * Each input is treated as a whole .heic/.heif/.avif file: open it, probe
 * info, decode primary (heic_doc_decode or heic_doc_decode_into) + thumbnail
 * + gain map + sequence frames, pull EXIF/XMP/ICC so malformed bytes reach
 * the container, HEVC, unci, and (when linked) dav1d paths. Every input must
 * also leave the context allocator balanced.
 *
 * Drivers (same LLVMFuzzerTestOneInput, shared fuzz/corpus/):
 *   bun cmd/fuzz.ts      clang -fsanitize=address,fuzzer (libFuzzer main)
 *   bun cmd/fuzz-afl.ts  afl-clang-fast -fsanitize=fuzzer (AFL++ libAFLDriver)
 */
#include "heic.h"

#include <stdlib.h>
#include <string.h>

/* Budgeted allocator: crafted ispe/grid can declare huge canvases; bound live
 * bytes so those inputs fail cleanly via the library's alloc-failure paths
 * (fuzzing those too) instead of tripping libFuzzer's RSS limit. Each block
 * stores its size in a 16-byte header (keeps malloc alignment).
 * Covers the library's own allocations only: dav1d, zlib and brotli use the
 * C allocator directly. */
#define FUZZ_MEM_BUDGET ((size_t)256 << 20) /* 256 MB live per input */

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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    heic_ctx *ctx;
    heic_doc *doc;
    heic_image_info info;
    heic_sequence_info seq;
    heic_sequence_frame_info frame_info;
    heic_image *img;
    heic_limits lim;
    uint8_t *meta;
    size_t meta_len;

    if (size < 8) return 0;

    heic_init();

    fuzz_live = 0;
    ctx = heic_ctx_new(fuzz_alloc, fuzz_free, NULL, NULL);
    if (!ctx) return 0;

    /* Tighter than production defaults so one pathological file cannot stall
     * the fuzzer (library still enforces max_memory_bytes). */
    lim.max_width = 4096;
    lim.max_height = 4096;
    lim.max_pixels = 16ull * 1024 * 1024;
    lim.max_memory_bytes = FUZZ_MEM_BUDGET;
    heic_ctx_set_limits(ctx, &lim);

    doc = heic_doc_open(ctx, data, size);
    if (!doc) {
        heic_ctx_free(ctx);
        if (fuzz_live != 0) abort(); /* failed open must not leak either */
        return 0;
    }

    (void)heic_doc_kind(doc);
    /* A failed probe (e.g. primary without ispe) must not hide the rest:
       thumbnail, gain map, sequences and metadata do not depend on it. */
    if (heic_doc_info(doc, &info) != 0) memset(&info, 0, sizeof(info));

    {
        /* RGB is the common path; RGBA hits alpha attach when present. The
           last input byte picks the output order and the API: half the
           inputs go through heic_doc_decode_into with a padded stride. */
        int sel = data[size - 1];
        heic_format fmt = info.has_alpha
            ? ((sel & 2) ? HEIC_FORMAT_BGRA : HEIC_FORMAT_RGBA)
            : ((sel & 2) ? HEIC_FORMAT_BGR : HEIC_FORMAT_RGB);
        size_t tight = heic_doc_output_size(doc, fmt);
        /* Keep the caller buffer within the allocator budget. */
        if ((sel & 1) && tight && info.height &&
            tight <= FUZZ_MEM_BUDGET / 2) {
            int stride = (int)(tight / info.height) + 5;
            size_t cap = (size_t)stride * info.height;
            uint8_t *buf = (uint8_t *)malloc(cap);
            if (buf) {
                (void)heic_doc_decode_into(doc, fmt, buf, cap, stride);
                free(buf);
            }
        } else {
            img = heic_doc_decode(doc, fmt);
            if (img) heic_image_destroy(ctx, img);
        }
    }

    img = heic_doc_decode_thumbnail(doc, HEIC_FORMAT_RGB);
    if (img) heic_image_destroy(ctx, img);

    img = heic_doc_decode_gain_map(doc, HEIC_FORMAT_RGB);
    if (img) heic_image_destroy(ctx, img);

    if (heic_doc_sequence_info(doc, &seq) == 0 && seq.frame_count) {
        uint32_t frame = seq.frame_count <= 32 ? seq.frame_count - 1 : 0;
        heic_sequence_decoder *decoder;
        (void)heic_doc_sequence_frame_info(doc, 0, &frame_info);
        (void)heic_doc_sequence_frame_info(doc, frame, &frame_info);
        img = heic_doc_decode_sequence_frame(doc, frame, HEIC_FORMAT_RGBA);
        if (img) heic_image_destroy(ctx, img);
        decoder = heic_sequence_decoder_new(doc, HEIC_FORMAT_RGBA);
        if (decoder) {
            img = heic_sequence_decoder_decode_frame(decoder, 0);
            if (img) heic_image_destroy(ctx, img);
            img = heic_sequence_decoder_decode_frame(decoder, frame);
            if (img) heic_image_destroy(ctx, img);
            heic_sequence_decoder_reset(decoder);
            img = heic_sequence_decoder_decode_frame(decoder, 0);
            if (img) heic_image_destroy(ctx, img);
            heic_sequence_decoder_destroy(decoder);
        }
    }

    meta = NULL;
    meta_len = 0;
    if (heic_doc_exif(doc, &meta, &meta_len) && meta) heic_free(ctx, meta);
    meta = NULL;
    meta_len = 0;
    if (heic_doc_xmp(doc, &meta, &meta_len) && meta) heic_free(ctx, meta);
    meta = NULL;
    meta_len = 0;
    if (heic_doc_icc(doc, &meta, &meta_len) && meta) heic_free(ctx, meta);

    heic_doc_close(doc);
    heic_ctx_free(ctx);
    /* Everything is closed: a non-zero balance is a leak in the library
       (ASan has no leak detection on Windows, so check it here). */
    if (fuzz_live != 0) abort();
    return 0;
}
