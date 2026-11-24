/* fuzz_diff.c -- fuzz target "diff": our output against libheif's.
 *
 * The other targets only find crashes and leaks. This one finds wrong
 * pixels: every input is decoded by both libraries and, where BOTH succeed,
 * the primary image must have the same size and (nearly) the same RGB.
 *
 * libheif is only an oracle for inputs it decodes cleanly, so both sides run
 * strict: libheif with strict_decoding, we with strict_bitstream. A damaged
 * bitstream is then an error, not a concealed picture that nobody could be
 * expected to reproduce. Inputs one side rejects are not compared at all,
 * nor are files whose colour matrix or crop fractions are nonsense, or that
 * use predictive image coding (which libheif lacks). A finding is still a lead, not a verdict:
 * libheif has bugs and choices of its own (see test/no_libheif_oracles.json),
 * so triage each one with `heic_test -verify`.
 *
 *   bun cmd/fuzz.ts -target diff      (Linux / macOS; needs the libheif oracle)
 */
#include "fuzz_common.h"
#include "heic_internal.h"

#include <libheif/heif.h>

#include <stdio.h>

/* The corpus tests accept mse <= 8 against libheif (the two HEVC decoders
 * round a few filters differently). Mutated inputs sit further out, so the
 * fuzzer only reports clearly different pictures. */
#define FUZZ_DIFF_MAX_MSE 64.0

/* libheif's RGB for the primary image, tightly packed; NULL when it does not
 * decode the input cleanly. */
static uint8_t *diff_libheif_rgb(const uint8_t *data, size_t size, int *w,
                                 int *h)
{
    heif_context *hc = heif_context_alloc();
    heif_image_handle *handle = NULL;
    heif_image *image = NULL;
    heif_decoding_options *opts = NULL;
    uint8_t *out = NULL;
    heif_error err;
    const uint8_t *plane;
    int stride = 0, y;

    if (!hc) return NULL;
    heif_context_set_max_decoding_threads(hc, 1);
    heif_context_set_maximum_image_size_limit(hc, 4096);
    err = heif_context_read_from_memory_without_copy(hc, data, size, NULL);
    if (err.code != heif_error_Ok) goto done;
    err = heif_context_get_primary_image_handle(hc, &handle);
    if (err.code != heif_error_Ok || !handle) goto done;
    *w = heif_image_handle_get_width(handle);
    *h = heif_image_handle_get_height(handle);
    if (*w <= 0 || *h <= 0 || (uint64_t)*w * (uint64_t)*h > FUZZ_MAX_PIXELS)
        goto done;
    opts = heif_decoding_options_alloc();
    if (!opts) goto done;
    opts->strict_decoding = 1;
    err = heif_decode_image(handle, &image, heif_colorspace_RGB,
                            heif_chroma_interleaved_RGB, opts);
    if (err.code != heif_error_Ok || !image) goto done;
    /* The decoded size is what counts (the handle's can be stale). */
    *w = heif_image_get_primary_width(image);
    *h = heif_image_get_primary_height(image);
    plane = heif_image_get_plane_readonly(image, heif_channel_interleaved,
                                          &stride);
    if (!plane || *w <= 0 || *h <= 0) goto done;
    out = (uint8_t *)malloc((size_t)*w * (size_t)*h * 3);
    if (!out) goto done;
    for (y = 0; y < *h; y++)
        memcpy(out + (size_t)y * (size_t)*w * 3, plane + (size_t)y * stride,
               (size_t)*w * 3);
done:
    if (opts) heif_decoding_options_free(opts);
    if (image) heif_image_release(image);
    if (handle) heif_image_handle_release(handle);
    heif_context_free(hc);
    return out;
}

/* Clean-aperture boxes whose fractions are not plain numbers: whole sizes,
 * offsets in halves, all small. Mutation turns them into values like
 * 1714124909/1811939330, which libheif's 32-bit rational arithmetic and ours
 * round differently; nothing is learned from comparing those. */
static int diff_clap_sane(const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 4; i + 36 <= size; i++) {
        const uint8_t *p = data + i;
        int k;
        if (p[0] != 'c' || p[1] != 'l' || p[2] != 'a' || p[3] != 'p') continue;
        for (k = 0; k < 8; k++) {
            const uint8_t *q = p + 4 + 4 * k;
            int32_t v = (int32_t)(((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16)
                                  | ((uint32_t)q[2] << 8) | q[3]);
            if (k & 1) { /* denominator: 1, or 2 for the offsets */
                if (v != 1 && !(k >= 5 && v == 2)) return 0;
            } else if (v < -65536 || v > 65536) {
                return 0;
            }
        }
    }
    return 1;
}

/* Predictively coded images ('pred' references, 'rref' property): libheif
 * does not implement them, and decodes such an item on its own when a
 * mutation hides the reference from it. */
static int diff_has_pred(const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i + 4 <= size; i++) {
        const uint8_t *p = data + i;
        if ((p[0] == 'p' && p[1] == 'r' && p[2] == 'e' && p[3] == 'd')
            || (p[0] == 'r' && p[1] == 'r' && p[2] == 'e' && p[3] == 'f'))
            return 1;
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    heic_ctx *ctx;
    heic_doc *doc;
    heic_image *img;
    heic_image_info info;
    uint8_t *ref;
    int rw = 0, rh = 0, comparable;

    if (size < 8) return 0;
    ctx = fuzz_ctx_new();
    if (!ctx) return 0;
    /* Our side is strict as well: slice data that runs out is an error, not
       a picture padded with whatever the decoder does past the end. */
    ctx->strict_bitstream = 1;
    doc = heic_doc_open(ctx, data, size);
    if (!doc) {
        fuzz_ctx_free(ctx);
        return 0;
    }
    /* Pixels are only comparable when the file asks for a YCbCr matrix both
       libraries implement (identity, BT.709, BT.601, BT.2020 NCL, or none).
       Any other value -- which is what a mutated colr box usually holds --
       makes each side fall back in its own way. */
    comparable = heic_doc_info(doc, &info) == 0
                 && diff_clap_sane(data, size)
                 && !diff_has_pred(data, size)
                 && (info.matrix_coefficients <= 2
                     || info.matrix_coefficients == 5
                     || info.matrix_coefficients == 6
                     || info.matrix_coefficients == 9);
    img = heic_doc_decode(doc, HEIC_FORMAT_RGB);
    /* libheif is the slow side: only ask it when we produced a picture. */
    ref = img && comparable ? diff_libheif_rgb(data, size, &rw, &rh) : NULL;
    if (img && ref) {
        if ((int)img->width != rw || (int)img->height != rh) {
            fprintf(stderr, "diff: size %ux%u, libheif %dx%d\n",
                    (unsigned)img->width, (unsigned)img->height, rw, rh);
            abort();
        } else {
            double sum = 0;
            size_t n = (size_t)rw * 3;
            int y;
            for (y = 0; y < rh; y++) {
                const uint8_t *a = img->data + (size_t)y * (size_t)img->stride;
                const uint8_t *b = ref + (size_t)y * n;
                size_t x;
                for (x = 0; x < n; x++) {
                    int d = (int)a[x] - (int)b[x];
                    sum += (double)(d * d);
                }
            }
            sum /= (double)n * (double)rh;
            if (sum > FUZZ_DIFF_MAX_MSE) {
                fprintf(stderr, "diff: %dx%d mse %.2f vs libheif\n", rw, rh,
                        sum);
                abort();
            }
        }
    }
    free(ref);
    if (img) heic_image_destroy(ctx, img);
    heic_doc_close(doc);
    fuzz_ctx_free(ctx);
    return 0;
}
