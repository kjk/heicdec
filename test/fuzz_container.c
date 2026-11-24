/* fuzz_container.c -- fuzz target "container": ISOBMFF / HEIF parsing only.
 *
 * Opens the input and asks for everything that needs no pixel decoding:
 * image info, output sizes, sequence tables and EXIF/XMP/ICC. Without the
 * codecs an execution costs microseconds instead of milliseconds, so the box
 * parsers (meta, iloc, iinf, iprp, iref, moov, ...) get orders of magnitude
 * more mutations than through the whole-file target (fuzz_target.c).
 *
 *   bun cmd/fuzz.ts -target container
 */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const heic_format formats[] = {
        HEIC_FORMAT_RGB, HEIC_FORMAT_BGR, HEIC_FORMAT_RGBA, HEIC_FORMAT_BGRA
    };
    heic_ctx *ctx;
    heic_doc *doc;
    heic_image_info info;
    heic_sequence_info seq;
    heic_sequence_frame_info frame_info;
    uint8_t *meta;
    size_t meta_len, i;

    if (size < 8) return 0;
    ctx = fuzz_ctx_new();
    if (!ctx) return 0;
    doc = heic_doc_open(ctx, data, size);
    if (!doc) {
        fuzz_ctx_free(ctx);
        return 0;
    }

    (void)heic_doc_kind(doc);
    (void)heic_doc_info(doc, &info);
    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        (void)heic_doc_output_size(doc, formats[i]);

    if (heic_doc_sequence_info(doc, &seq) == 0 && seq.frame_count) {
        /* The first, the last and a few in between: the sample tables are
           walked per frame. */
        uint32_t n = seq.frame_count, step = n > 16 ? n / 16 : 1, f;
        for (f = 0; f < n; f += step)
            (void)heic_doc_sequence_frame_info(doc, f, &frame_info);
        (void)heic_doc_sequence_frame_info(doc, n - 1, &frame_info);
        (void)heic_doc_sequence_frame_info(doc, n, &frame_info); /* one past */
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
    fuzz_ctx_free(ctx);
    return 0;
}
