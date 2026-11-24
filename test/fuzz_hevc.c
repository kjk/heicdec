/* fuzz_hevc.c -- fuzz target "hevc": the HEVC decoder without the container.
 *
 * The input is a raw Annex B stream (start codes + NAL units), like the
 * conformance streams. Parameter sets are collected as they appear, each
 * access unit is decoded with the pictures decoded so far as references.
 * Every mutated byte lands in a parameter set, slice header or slice data,
 * instead of mostly breaking the box structure around them.
 *
 *   bun cmd/fuzz.ts -target hevc
 */
#include "fuzz_common.h"
#include "heic_internal.h"

#define FUZZ_HEVC_MAX_NALS 512
#define FUZZ_HEVC_MAX_PARAMS 24
#define FUZZ_HEVC_MAX_FRAMES 4 /* access units decoded per input */
/* Small pictures only (up to 416x240, the size of most conformance streams):
   the decoder's code paths do not depend on the picture size, its run time
   does. */
#define FUZZ_HEVC_MAX_PIXELS ((uint64_t)1 << 17)

typedef struct {
    const uint8_t *data;
    size_t len;
    uint8_t type;
} fuzz_nal;

static int fuzz_find_start_code(const uint8_t *data, size_t len, size_t from,
                                size_t *pos, size_t *prefix)
{
    size_t i;
    for (i = from; i + 3 <= len; i++) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        if (data[i + 2] == 1) {
            *pos = i;
            *prefix = 3;
            return 1;
        }
        if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
            *pos = i;
            *prefix = 4;
            return 1;
        }
    }
    return 0;
}

static int fuzz_split_annexb(const uint8_t *data, size_t len, fuzz_nal *nals,
                             int cap)
{
    size_t start, prefix, next, next_prefix;
    int n = 0;
    if (!fuzz_find_start_code(data, len, 0, &start, &prefix)) return 0;
    for (;;) {
        size_t nal_start = start + prefix;
        size_t nal_end;
        int have_next = fuzz_find_start_code(data, len, nal_start + 2, &next,
                                             &next_prefix);
        nal_end = have_next ? next : len;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        if (nal_end >= nal_start + 2 && n < cap) {
            nals[n].data = data + nal_start;
            nals[n].len = nal_end - nal_start;
            nals[n].type = (uint8_t)((data[nal_start] >> 1) & 0x3f);
            n++;
        }
        if (!have_next || n >= cap) break;
        start = next;
        prefix = next_prefix;
    }
    return n;
}

/* VCL NAL with first_slice_segment_in_pic_flag set: starts an access unit. */
static int fuzz_first_slice(const fuzz_nal *nal)
{
    return nal->type <= 31 && nal->len > 2 && (nal->data[2] & 0x80) != 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fuzz_nal nals[FUZZ_HEVC_MAX_NALS];
    uint8_t *params[FUZZ_HEVC_MAX_PARAMS];
    size_t param_lens[FUZZ_HEVC_MAX_PARAMS];
    heic_frame frames[FUZZ_HEVC_MAX_FRAMES];
    const heic_frame *refs[FUZZ_HEVC_MAX_FRAMES];
    heic_hvcc cfg;
    heic_ctx *ctx;
    int n_nals, n_params = 0, n_frames = 0, i = 0, k;

    if (size < 8) return 0;
    n_nals = fuzz_split_annexb(data, size, nals, FUZZ_HEVC_MAX_NALS);
    if (n_nals <= 0) return 0;
    ctx = fuzz_ctx_new_limited(FUZZ_HEVC_MAX_PIXELS);
    if (!ctx) return 0;

    memset(&cfg, 0, sizeof(cfg));
    memset(frames, 0, sizeof(frames));
    cfg.length_size_minus_one = 3;
    cfg.nal_units = params;
    cfg.nal_unit_lens = param_lens;

    while (i < n_nals && n_frames < FUZZ_HEVC_MAX_FRAMES) {
        uint8_t *sample;
        size_t sample_len = 0, pos = 0;
        int au_start, au_end;

        /* Parameter sets seen before this access unit are available to it.
           The decoder parses them in order, so a later one with the same id
           replaces an earlier one. */
        while (i < n_nals && nals[i].type > 31) {
            if (nals[i].type >= 32 && nals[i].type <= 34
                && n_params < FUZZ_HEVC_MAX_PARAMS) {
                params[n_params] = (uint8_t *)nals[i].data;
                param_lens[n_params] = nals[i].len;
                n_params++;
            }
            i++;
        }
        if (i >= n_nals) break;
        au_start = i++;
        while (i < n_nals && !fuzz_first_slice(&nals[i])) i++;
        au_end = i;

        for (k = au_start; k < au_end; k++)
            if (nals[k].type <= 31) sample_len += nals[k].len + 4;
        sample = (uint8_t *)malloc(sample_len ? sample_len : 1);
        if (!sample) break;
        for (k = au_start; k < au_end; k++) {
            size_t n = nals[k].len;
            if (nals[k].type > 31) continue;
            sample[pos++] = (uint8_t)(n >> 24);
            sample[pos++] = (uint8_t)(n >> 16);
            sample[pos++] = (uint8_t)(n >> 8);
            sample[pos++] = (uint8_t)n;
            memcpy(sample + pos, nals[k].data, n);
            pos += n;
        }

        cfg.n_nal_units = n_params;
        for (k = 0; k < n_frames; k++) refs[k] = &frames[n_frames - 1 - k];
        /* A failed picture is dropped and decoding goes on with the next
           access unit: error paths inside a sequence matter too. */
        if (heic_hevc_decode_refs(ctx, &cfg, sample, sample_len, refs,
                                  n_frames, &frames[n_frames], NULL) == 0) {
            /* A decoded picture has every sample written (see FUZZ_CHECK_INIT). */
            const heic_frame *f = &frames[n_frames];
            int y;
            for (y = 0; y < f->height; y++)
                FUZZ_CHECK_INIT(f->y + (size_t)y * (size_t)f->y_stride,
                                (size_t)f->width * sizeof(uint16_t));
            for (y = 0; f->chroma_format != 0 && y < f->c_height; y++) {
                FUZZ_CHECK_INIT(f->cb + (size_t)y * (size_t)f->c_stride,
                                (size_t)f->c_width * sizeof(uint16_t));
                FUZZ_CHECK_INIT(f->cr + (size_t)y * (size_t)f->c_stride,
                                (size_t)f->c_width * sizeof(uint16_t));
            }
            n_frames++;
        } else
            heic_frame_free(ctx, &frames[n_frames]);
        free(sample);

        /* Parameter sets inside the access unit apply from the next one. */
        for (k = au_start + 1; k < au_end; k++)
            if (nals[k].type >= 32 && nals[k].type <= 34
                && n_params < FUZZ_HEVC_MAX_PARAMS) {
                params[n_params] = (uint8_t *)nals[k].data;
                param_lens[n_params] = nals[k].len;
                n_params++;
            }
    }

    for (k = 0; k < n_frames; k++) heic_frame_free(ctx, &frames[k]);
    fuzz_ctx_free(ctx);
    return 0;
}
