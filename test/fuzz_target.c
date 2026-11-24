/* fuzz_target.c -- libFuzzer / AFL++ entry point for the HEIC/HEIF/AVIF decoder.
 *
 * Each input is treated as a whole .heic/.heif/.avif file: open it, probe
 * info, decode primary (heic_doc_decode or heic_doc_decode_into) + thumbnail
 * + gain map + sequence frames, pull EXIF/XMP/ICC so malformed bytes reach
 * the container, HEVC, unci, and (when linked) dav1d paths. Every input must
 * also leave the context allocator balanced.
 *
 * This is the "heic" target. The others: fuzz_container.c (parsing only),
 * fuzz_hevc.c (raw HEVC streams), fuzz_diff.c (output vs libheif).
 *
 * Drivers (same LLVMFuzzerTestOneInput, shared fuzz/corpus/):
 *   bun cmd/fuzz.ts      clang -fsanitize=address,fuzzer (libFuzzer main)
 *   bun cmd/fuzz-afl.ts  afl-clang-fast -fsanitize=fuzzer (AFL++ libAFLDriver)
 */
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    heic_ctx *ctx;
    heic_doc *doc;
    heic_image_info info;
    heic_sequence_info seq;
    heic_sequence_frame_info frame_info;
    heic_image *img;
    uint8_t *meta;
    size_t meta_len;

    if (size < 8) return 0;

    /* Limits tighter than production (2 Mpx, see fuzz_common.h) so one
     * pathological file cannot stall the fuzzer. */
    ctx = fuzz_ctx_new();
    if (!ctx) return 0;

    doc = heic_doc_open(ctx, data, size);
    if (!doc) {
        fuzz_ctx_free(ctx); /* a failed open must not leak either */
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
                if (heic_doc_decode_into(doc, fmt, buf, cap, stride) == 0)
                    fuzz_check_pixels(buf, info.width, info.height, stride,
                                      fmt);
                free(buf);
            }
        } else {
            img = heic_doc_decode(doc, fmt);
            if (img) {
                fuzz_check_image(img);
                heic_image_destroy(ctx, img);
            }
        }
    }

    img = heic_doc_decode_thumbnail(doc, HEIC_FORMAT_RGB);
    if (img) {
                fuzz_check_image(img);
                heic_image_destroy(ctx, img);
            }

    img = heic_doc_decode_gain_map(doc, HEIC_FORMAT_RGB);
    if (img) {
                fuzz_check_image(img);
                heic_image_destroy(ctx, img);
            }

    if (heic_doc_sequence_info(doc, &seq) == 0 && seq.frame_count) {
        uint32_t frame = seq.frame_count <= 32 ? seq.frame_count - 1 : 0;
        heic_sequence_decoder *decoder;
        (void)heic_doc_sequence_frame_info(doc, 0, &frame_info);
        (void)heic_doc_sequence_frame_info(doc, frame, &frame_info);
        img = heic_doc_decode_sequence_frame(doc, frame, HEIC_FORMAT_RGBA);
        if (img) {
                fuzz_check_image(img);
                heic_image_destroy(ctx, img);
            }
        decoder = heic_sequence_decoder_new(doc, HEIC_FORMAT_RGBA);
        if (decoder) {
            img = heic_sequence_decoder_decode_frame(decoder, 0);
            if (img) {
                fuzz_check_image(img);
                heic_image_destroy(ctx, img);
            }
            img = heic_sequence_decoder_decode_frame(decoder, frame);
            if (img) {
                fuzz_check_image(img);
                heic_image_destroy(ctx, img);
            }
            heic_sequence_decoder_reset(decoder);
            img = heic_sequence_decoder_decode_frame(decoder, 0);
            if (img) {
                fuzz_check_image(img);
                heic_image_destroy(ctx, img);
            }
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
    fuzz_ctx_free(ctx);
    return 0;
}
