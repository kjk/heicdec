/* heic_test.c -- CLI harness (jbig2dec / djvudec style)
 *
 *   heic_test -info file.heic
 *   heic_test -out out.ppm file.heic
 *   heic_test -exif file.heic
 *   heic_test -bench file.heic   # needs HEIC_HAVE_LIBHEIF (libheif + libde265)
 *   heic_test -verify file.heic  # pixel MSE vs libheif (optional)
 *   heic_test -profile-heic N file.heic     # loop decode N times (winperf marks)
 *   heic_test -profile-libheif N file.heic  # same for libheif (needs oracle)
 *   heic_test -hevc-sequence file.bit       # decode every Annex B access unit
 */
#include "heic.h"
#include "heic_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HEIC_HAVE_LIBHEIF
#include "bench_libheif.h"
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include "bench_wic.h"
/* Vendored control client; calls are no-ops when winperf is not recording. */
#include "winperf_control.h"
#else
#include <time.h>
static void winperf_profile_start(void) {}
static void winperf_profile_stop(void) {}
#endif

static void on_error(void *user, heic_severity sev, const char *msg)
{
    const char *s = "?";
    (void)user;
    switch (sev) {
    case HEIC_SEVERITY_DEBUG: s = "debug"; break;
    case HEIC_SEVERITY_INFO: s = "info"; break;
    case HEIC_SEVERITY_WARNING: s = "warn"; break;
    case HEIC_SEVERITY_ERROR: s = "error"; break;
    case HEIC_SEVERITY_FATAL: s = "fatal"; break;
    }
    fprintf(stderr, "heic[%s]: %s\n", s, msg ? msg : "");
}

static void on_error_quiet(void *user, heic_severity sev, const char *msg)
{
    (void)user;
    (void)sev;
    (void)msg;
}

static void on_error_only(void *user, heic_severity sev, const char *msg)
{
    if (sev >= HEIC_SEVERITY_ERROR) on_error(user, sev, msg);
}

static double bench_now_ms(void)
{
#ifdef HEIC_HAVE_LIBHEIF
    return heic_bench_now_ms();
#elif defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    /* C11 timespec_get — no POSIX feature macros needed under -std=c11. */
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f;
    long sz;
    uint8_t *buf;
    size_t n;
    f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = n;
    return buf;
}

typedef struct {
    const uint8_t *data;
    size_t len;
    uint8_t type;
} annexb_nal;

static int find_start_code(const uint8_t *data, size_t len, size_t from,
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

static int split_annexb(const uint8_t *data, size_t len,
                        annexb_nal *nals, int cap)
{
    size_t start, prefix, next, next_prefix;
    int n = 0;
    if (!find_start_code(data, len, 0, &start, &prefix)) return 0;
    for (;;) {
        size_t nal_start = start + prefix;
        size_t nal_end;
        int have_next = find_start_code(
            data, len, nal_start + 2, &next, &next_prefix);
        nal_end = have_next ? next : len;
        while (nal_end > nal_start && data[nal_end - 1] == 0) nal_end--;
        if (nal_end >= nal_start + 2) {
            if (nals && n < cap) {
                nals[n].data = data + nal_start;
                nals[n].len = nal_end - nal_start;
                nals[n].type = (uint8_t)((data[nal_start] >> 1) & 0x3f);
            }
            n++;
        }
        if (!have_next) break;
        start = next;
        prefix = next_prefix;
    }
    return n;
}

static int annexb_first_slice(const annexb_nal *nal)
{
    return nal && nal->type <= 31 && nal->len > 2
        && (nal->data[2] & 0x80) != 0;
}

static int update_hvcc_param(uint8_t **params, size_t *param_lens,
                             int *n_params, const annexb_nal *nal)
{
    int i;
    if (!params || !param_lens || !n_params || !nal ||
        nal->type < 32 || nal->type > 34)
        return 0;
    /* Keep every SPS (streams like SLIST_B carry SPS id 0 with scaling lists
       plus a second SPS id 1). Replace VPS and the active PPS in place. */
    if (nal->type != 33) {
        for (i = 0; i < *n_params; i++) {
            uint8_t type = (uint8_t)((params[i][0] >> 1) & 0x3f);
            if (type == nal->type) {
                params[i] = (uint8_t *)nal->data;
                param_lens[i] = nal->len;
                return 0;
            }
        }
    } else {
        for (i = 0; i < *n_params; i++) {
            uint8_t type = (uint8_t)((params[i][0] >> 1) & 0x3f);
            if (type == 33 && params[i] == nal->data
                && param_lens[i] == nal->len)
                return 0;
        }
    }
    if (*n_params >= 16) return -1;
    params[*n_params] = (uint8_t *)nal->data;
    param_lens[*n_params] = nal->len;
    (*n_params)++;
    return 0;
}

static uint64_t hash_plane(uint64_t h, const uint16_t *p,
                           int stride, int w, int height)
{
    int x, y;
    for (y = 0; y < height; y++)
        for (x = 0; x < w; x++) {
            uint16_t v = p[(size_t)y * stride + x];
            h = (h ^ (uint8_t)v) * UINT64_C(1099511628211);
            h = (h ^ (uint8_t)(v >> 8)) * UINT64_C(1099511628211);
        }
    return h;
}

/* Frames -hevc-sequence may hold at once (references + frames waiting for
   output); the stream itself can be any length. */
#define HEVC_SEQ_MAX_LIVE 512

/* Hash one output frame and, when `file` is set, append it as raw planar
   YUV (conformance-window cropped). Returns -1 on a write error. */
static int hevc_seq_emit(const heic_frame *fr, FILE *file, uint64_t *hash)
{
    int plane, y, x;
    int sub_x = fr->chroma_format == 3 ? 1 : (fr->chroma_format == 0 ? 1 : 2);
    int sub_y = fr->chroma_format == 1 ? 2 : 1;
    const uint16_t *planes[3] = {fr->y, fr->cb, fr->cr};
    int strides[3] = {fr->y_stride, fr->c_stride, fr->c_stride};
    int origins[3][2] = {
        { fr->crop_left, fr->crop_top },
        { fr->crop_left / sub_x, fr->crop_top / sub_y },
        { fr->crop_left / sub_x, fr->crop_top / sub_y }
    };
    int widths[3] = {
        fr->width - fr->crop_left - fr->crop_right,
        fr->c_width - fr->crop_left / sub_x - fr->crop_right / sub_x,
        fr->c_width - fr->crop_left / sub_x - fr->crop_right / sub_x
    };
    int heights[3] = {
        fr->height - fr->crop_top - fr->crop_bottom,
        fr->c_height - fr->crop_top / sub_y - fr->crop_bottom / sub_y,
        fr->c_height - fr->crop_top / sub_y - fr->crop_bottom / sub_y
    };
    int depths[3] = {
        fr->bit_depth, fr->chroma_bit_depth, fr->chroma_bit_depth
    };
    int n_planes = fr->chroma_format ? 3 : 1;
    /* Like the HM reference decoder (whose MD5s ship with the conformance
       streams): one sample size for the whole file, 16-bit words as soon as
       any plane is deeper than 8 bits. */
    int wide = depths[0] > 8 || (n_planes > 1 && depths[1] > 8);
    int rc = 0;
    if (widths[0] <= 0 || heights[0] <= 0) return -1;
    for (plane = 0; plane < n_planes; plane++) {
        const uint16_t *p = planes[plane]
            + (size_t)origins[plane][1] * strides[plane] + origins[plane][0];
        *hash = hash_plane(*hash, p, strides[plane], widths[plane],
                           heights[plane]);
        if (!file) continue;
        for (y = 0; y < heights[plane]; y++) {
            const uint16_t *row = p + (size_t)y * strides[plane];
            uint8_t buf[4096];
            size_t n = 0;
            for (x = 0; x < widths[plane]; x++) {
                buf[n++] = (uint8_t)row[x];
                if (wide) buf[n++] = (uint8_t)(row[x] >> 8);
                if (n >= sizeof(buf) - 1 || x == widths[plane] - 1) {
                    if (fwrite(buf, 1, n, file) != n) rc = -1;
                    n = 0;
                }
            }
        }
    }
    return rc;
}

static int do_hevc_sequence(const uint8_t *data, size_t len,
                            const char *out_path, int max_frames)
{
    annexb_nal *nals = NULL;
    uint8_t *params[16];
    size_t param_lens[16];
    heic_hvcc cfg;
    heic_frame frames[HEVC_SEQ_MAX_LIVE]; /* held frames, decode order */
    uint8_t pending[HEVC_SEQ_MAX_LIVE];   /* still waiting to be output */
    const heic_frame *refs[HEVC_SEQ_MAX_LIVE];
    heic_ctx *ctx;
    FILE *out_file = NULL;
    int n_nals, n_params = 0, decoded = 0, n_live = 0, n_out = 0, i, j;
    int complete = 1;
    uint64_t hash = UINT64_C(1469598103934665603);

    memset(&cfg, 0, sizeof(cfg));
    memset(frames, 0, sizeof(frames));
    n_nals = split_annexb(data, len, NULL, 0);
    if (n_nals <= 0) {
        fprintf(stderr, "hevc-sequence: no Annex B NAL units\n");
        return 1;
    }
    if ((size_t)n_nals > SIZE_MAX / sizeof(*nals)) return 1;
    nals = (annexb_nal *)malloc((size_t)n_nals * sizeof(*nals));
    if (!nals) return 1;
    if (split_annexb(data, len, nals, n_nals) != n_nals) {
        free(nals);
        return 1;
    }
    for (i = 0; i < n_nals && nals[i].type > 31; i++)
        if (update_hvcc_param(params, param_lens, &n_params, &nals[i]) != 0) {
            free(nals);
            return 1;
        }
    if (n_params < 2) {
        fprintf(stderr, "hevc-sequence: missing parameter sets\n");
        free(nals);
        return 1;
    }
    cfg.length_size_minus_one = 3;
    cfg.nal_units = params;
    cfg.nal_unit_lens = param_lens;
    cfg.n_nal_units = n_params;
    ctx = heic_ctx_new(NULL, NULL, on_error_only, NULL);
    if (!ctx) {
        free(nals);
        return 1;
    }
    {
        /* Up to HEVC_SEQ_MAX_LIVE frames can be held (a stream that never
           releases references keeps them all), which the default 1 GiB
           context limit does not cover for large pictures. */
        heic_limits lim;
        memset(&lim, 0, sizeof(lim));
        lim.max_memory_bytes = (uint64_t)16 << 30;
        heic_ctx_set_limits(ctx, &lim);
    }

    i = 0;
    if (out_path) {
        out_file = fopen(out_path, "wb");
        if (!out_file) complete = 0;
    }
    {
    int after_eos = 0; /* next CRA has NoRaslOutputFlag (H.265 8.1.3) */
    int suppress_rasl = 0;
    uint16_t cvs = 0;
    while (complete && i < n_nals && decoded < max_frames) {
        uint8_t *sample;
        size_t sample_len = 0, pos = 0;
        heic_frame out;
        int au_start, au_end, k, n_refs;
        while (i < n_nals && nals[i].type > 31) {
            if (nals[i].type == HEIC_NAL_EOS || nals[i].type == HEIC_NAL_EOB)
                after_eos = 1;
            i++;
        }
        if (i >= n_nals) break;
        au_start = i++;
        while (i < n_nals && !annexb_first_slice(&nals[i])) {
            if (nals[i].type == HEIC_NAL_EOS || nals[i].type == HEIC_NAL_EOB)
                after_eos = 1;
            i++;
        }
        au_end = i;
        for (k = au_start; k < au_end; k++) {
            if (nals[k].type > 31) continue;
            if (nals[k].len > SIZE_MAX - sample_len - 4) {
                complete = 0;
                break;
            }
            sample_len += nals[k].len + 4;
        }
        if (!complete) break;
        if (n_live >= HEVC_SEQ_MAX_LIVE) {
            fprintf(stderr, "hevc-sequence: more than %d frames held\n",
                    HEVC_SEQ_MAX_LIVE);
            complete = 0;
            break;
        }
        sample = (uint8_t *)malloc(sample_len);
        if (!sample) {
            complete = 0;
            break;
        }
        for (k = au_start; k < au_end; k++) {
            size_t nal_len;
            if (nals[k].type > 31) continue;
            nal_len = nals[k].len;
            sample[pos++] = (uint8_t)(nal_len >> 24);
            sample[pos++] = (uint8_t)(nal_len >> 16);
            sample[pos++] = (uint8_t)(nal_len >> 8);
            sample[pos++] = (uint8_t)nal_len;
            memcpy(sample + pos, nals[k].data, nal_len);
            pos += nal_len;
        }
        /* Every frame still held is a candidate reference, newest first. */
        n_refs = n_live;
        cfg.n_nal_units = n_params;
        for (j = 0; j < n_refs; j++) refs[j] = &frames[n_live - 1 - j];
        memset(&out, 0, sizeof(out));
        if (heic_hevc_decode_refs(ctx, &cfg, sample, sample_len,
                                  refs, n_refs, &out, NULL) != 0) {
            fprintf(stderr, "hevc-sequence: frame %d decode failed\n",
                    decoded);
            free(sample);
            complete = 0;
            break;
        }
        free(sample);
        /* NoRaslOutputFlag (H.265 8.1.3): BLA always; CRA if first picture or
           first picture after EOS/EOB. IDR always starts a new CVS. */
        if (out.nal_unit_type >= HEIC_NAL_BLA_W_LP
            && out.nal_unit_type <= HEIC_NAL_BLA_N_LP)
            out.no_rasl_output_flag = 1;
        else if (out.nal_unit_type == HEIC_NAL_CRA)
            out.no_rasl_output_flag =
                (decoded == 0 || after_eos) ? 1u : 0u;
        else if (out.nal_unit_type == HEIC_NAL_IDR_W_RADL
                 || out.nal_unit_type == HEIC_NAL_IDR_N_LP)
            out.no_rasl_output_flag = 1;
        if (out.nal_unit_type >= HEIC_NAL_BLA_W_LP
            && out.nal_unit_type <= HEIC_NAL_CRA)
            after_eos = 0;
        {
            uint8_t nt = out.nal_unit_type;
            int is_rasl = (nt == HEIC_NAL_RASL_N || nt == HEIC_NAL_RASL_R);
            int is_irap = (nt >= HEIC_NAL_BLA_W_LP && nt <= HEIC_NAL_CRA);
            /* New CVS at IRAP with NoRaslOutputFlag (includes IDR/BLA and
               first/post-EOS CRA). Keeps the (cvs, POC) output order from
               interleaving CVSs that reuse the same POC values. */
            if (is_irap && out.no_rasl_output_flag) {
                if (decoded > 0) cvs++;
                suppress_rasl = 1;
            } else if (is_irap) {
                suppress_rasl = 0;
            }
            out.cvs_id = cvs;
            pending[n_live] =
                (uint8_t)(out.pic_output_flag && !(is_rasl && suppress_rasl));
        }
        frames[n_live++] = out;
        decoded++;
        for (k = au_start + 1; k < au_end; k++)
            if (update_hvcc_param(params, param_lens, &n_params, &nals[k]) != 0) {
                complete = 0;
                break;
            }
        if (!complete) break;

        /* Output in (cvs, POC) order as soon as it is safe: at most 15
           pictures may follow a picture in decoding order and precede it in
           output order (sps_max_num_reorder_pics), so with more than 16
           waiting the smallest one cannot be overtaken any more. */
        for (;;) {
            int waiting = 0, best = -1;
            for (j = 0; j < n_live; j++) {
                if (!pending[j]) continue;
                waiting++;
                if (best < 0
                    || frames[j].cvs_id < frames[best].cvs_id
                    || (frames[j].cvs_id == frames[best].cvs_id
                        && frames[j].poc < frames[best].poc))
                    best = j;
            }
            if (waiting <= 16) break;
            if (hevc_seq_emit(&frames[best], out_file, &hash) != 0) complete = 0;
            pending[best] = 0;
            n_out++;
        }
        /* Drop frames that were output and that no later picture can
           reference, except the newest POC anchor. Long streams would
           otherwise need every frame in memory. */
        {
            int anchor_seen = 0, w;
            for (j = n_live - 1; j >= 0; j--) {
                int anchor = heic_hevc_frame_is_poc_anchor(&frames[j]);
                if (!pending[j] && j != n_live - 1
                    && heic_hevc_frame_unreferenced(&frames[j])
                    && (!anchor || anchor_seen)) {
                    heic_frame_free(ctx, &frames[j]);
                    frames[j].width = 0; /* marks the slot as freed */
                }
                if (anchor) anchor_seen = 1;
            }
            for (j = 0, w = 0; j < n_live; j++) {
                if (frames[j].width == 0) continue;
                if (w != j) {
                    frames[w] = frames[j];
                    pending[w] = pending[j];
                }
                w++;
            }
            n_live = w;
        }
    }
    } /* after_eos scope */
    complete = complete && (i >= n_nals || decoded == max_frames);
    /* Flush what is still waiting, in (cvs, POC) order. */
    for (;;) {
        int best = -1;
        for (j = 0; j < n_live; j++) {
            if (!pending[j]) continue;
            if (best < 0
                || frames[j].cvs_id < frames[best].cvs_id
                || (frames[j].cvs_id == frames[best].cvs_id
                    && frames[j].poc < frames[best].poc))
                best = j;
        }
        if (best < 0) break;
        if (complete && hevc_seq_emit(&frames[best], out_file, &hash) != 0)
            complete = 0;
        pending[best] = 0;
        n_out++;
    }
    if (out_file && fclose(out_file) != 0) complete = 0;
    for (i = 0; i < n_live; i++) heic_frame_free(ctx, &frames[i]);
    heic_ctx_free(ctx);
    free(nals);
    if (n_out == 0 || !complete) return 1;
    printf("hevc-sequence frames=%d hash=%016llx\n", n_out,
           (unsigned long long)hash);
    return 0;
}

static int write_ppm(const char *path, const heic_image *img)
{
    FILE *f;
    int y;
    if (!img) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    if (img->format == HEIC_FORMAT_RGB) {
        fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
        for (y = 0; y < (int)img->height; y++) {
            const uint8_t *row = img->data + (size_t)y * (size_t)img->stride;
            if (fwrite(row, 1, (size_t)img->width * 3, f) != (size_t)img->width * 3) {
                fclose(f);
                return -1;
            }
        }
    } else if (img->format == HEIC_FORMAT_RGBA) {
        fprintf(f, "P6\n%d %d\n255\n", img->width, img->height);
        for (y = 0; y < (int)img->height; y++) {
            const uint8_t *row = img->data + (size_t)y * (size_t)img->stride;
            int x;
            for (x = 0; x < (int)img->width; x++) {
                if (fwrite(row + x * 4, 1, 3, f) != 3) {
                    fclose(f);
                    return -1;
                }
            }
        }
    } else {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static const char *kind_str(heic_kind k)
{
    switch (k) {
    case HEIC_KIND_HEIC: return "heic";
    case HEIC_KIND_AVIF: return "avif";
    case HEIC_KIND_HEIF: return "heif";
    case HEIC_KIND_SEQUENCE: return "sequence";
    default: return "unknown";
    }
}

typedef struct {
    double open_ms;
    double decode_ms;
    double close_ms;
    double total_ms;
    uint32_t width, height;
    int ok;
} bench_session;

static double bench_best2(double a, double b)
{
    if (a < 0.0) return b;
    if (b < 0.0) return a;
    return a < b ? a : b;
}

static double bench_best3(double a, double b, double c)
{
    return bench_best2(bench_best2(a, b), c);
}

static int bench_ours_session(const uint8_t *data, size_t len, bench_session *out)
{
    heic_ctx *ctx;
    heic_doc *doc;
    heic_image *img;
    heic_image_info info;
    double t0, t_session;

    memset(out, 0, sizeof(*out));
    out->open_ms = out->decode_ms = out->close_ms = out->total_ms = -1.0;

    t_session = bench_now_ms();
    ctx = heic_ctx_new(NULL, NULL, on_error_quiet, NULL);
    if (!ctx) return -1;

    t0 = bench_now_ms();
    doc = heic_doc_open(ctx, data, len);
    out->open_ms = bench_now_ms() - t0;
    if (!doc) {
        heic_ctx_free(ctx);
        return -1;
    }

    if (heic_doc_info(doc, &info) == 0) {
        out->width = info.width;
        out->height = info.height;
    }

    t0 = bench_now_ms();
    img = heic_doc_decode(doc, HEIC_FORMAT_RGB);
    out->decode_ms = bench_now_ms() - t0;
    if (!img) {
        heic_doc_close(doc);
        heic_ctx_free(ctx);
        return -1;
    }
    out->width = img->width;
    out->height = img->height;

    t0 = bench_now_ms();
    heic_image_destroy(ctx, img);
    heic_doc_close(doc);
    heic_ctx_free(ctx);
    out->close_ms = bench_now_ms() - t0;
    out->total_ms = bench_now_ms() - t_session;
    out->ok = 1;
    return 0;
}

static void bench_print_session_line(const char *tag, const bench_session *t)
{
    printf("%s open: %.2f decode: %.2f close: %.2f\n", tag, t->open_ms, t->decode_ms,
           t->close_ms);
}

#ifdef HEIC_HAVE_LIBHEIF
typedef struct {
    char op[16];
    double ours;
    double lib;
} bench_cmp_row;

static void bench_fmt_ms_cell(char *buf, size_t cap, double ms)
{
    if (ms < 0.0)
        snprintf(buf, cap, "ERROR");
    else
        snprintf(buf, cap, "%.2f", ms);
}

static void bench_fmt_diff_cell(char *buf, size_t cap, double ours, double lib)
{
    if (ours < 0.0 || lib < 0.0)
        snprintf(buf, cap, "ERROR");
    else
        snprintf(buf, cap, "%+.2f", ours - lib);
}

static void bench_fmt_pct_cell(char *buf, size_t cap, double ours, double lib)
{
    if (ours < 0.0 || lib < 0.0)
        snprintf(buf, cap, "ERROR");
    else if (lib > 0.0)
        snprintf(buf, cap, "%+.1f%%", (ours - lib) / lib * 100.0);
    else
        snprintf(buf, cap, "0.0%%");
}

static void bench_print_compare_table(const bench_cmp_row *rows, int nrows)
{
    static const char *h_op = "op";
    static const char *h_ours = "heic";
    static const char *h_lib = "libheif";
    static const char *h_diff = "diff";
    static const char *h_pct = "%diff";
    int w_op = (int)strlen(h_op);
    int w_ours = (int)strlen(h_ours);
    int w_lib = (int)strlen(h_lib);
    int w_diff = (int)strlen(h_diff);
    int w_pct = (int)strlen(h_pct);
    char ours[24], lib[24], diff[24], pct[24];
    int i, w;

    for (i = 0; i < nrows; i++) {
        bench_fmt_ms_cell(ours, sizeof ours, rows[i].ours);
        bench_fmt_ms_cell(lib, sizeof lib, rows[i].lib);
        bench_fmt_diff_cell(diff, sizeof diff, rows[i].ours, rows[i].lib);
        bench_fmt_pct_cell(pct, sizeof pct, rows[i].ours, rows[i].lib);
        w = (int)strlen(rows[i].op);
        if (w > w_op) w_op = w;
        w = (int)strlen(ours);
        if (w > w_ours) w_ours = w;
        w = (int)strlen(lib);
        if (w > w_lib) w_lib = w;
        w = (int)strlen(diff);
        if (w > w_diff) w_diff = w;
        w = (int)strlen(pct);
        if (w > w_pct) w_pct = w;
    }

    printf("%-*s %-*s %-*s %-*s %-*s\n", w_op, h_op, w_lib, h_lib, w_ours, h_ours,
           w_diff, h_diff, w_pct, h_pct);
    for (i = 0; i < nrows; i++) {
        bench_fmt_ms_cell(ours, sizeof ours, rows[i].ours);
        bench_fmt_ms_cell(lib, sizeof lib, rows[i].lib);
        bench_fmt_diff_cell(diff, sizeof diff, rows[i].ours, rows[i].lib);
        bench_fmt_pct_cell(pct, sizeof pct, rows[i].ours, rows[i].lib);
        printf("%-*s %-*s %-*s %-*s %-*s\n", w_op, rows[i].op, w_lib, lib, w_ours, ours,
               w_diff, diff, w_pct, pct);
    }
}
#endif

static int do_bench(const uint8_t *data, size_t len)
{
    const int RUNS = 3;
    bench_session ours[3];
#ifdef HEIC_HAVE_LIBHEIF
    heic_bench_session libraw[3];
    bench_session lib[3];
#endif
#ifdef _WIN32
    heic_wic_session wic[3];
    int com_inited = 0;
#endif
    int r;

    /* The session helpers can fail before touching their output (e.g. an
       empty file); the error strings below must not be read uninitialized. */
#ifdef HEIC_HAVE_LIBHEIF
    memset(libraw, 0, sizeof(libraw));
#endif
#ifdef _WIN32
    memset(wic, 0, sizeof(wic));
#endif

    printf("(bench: session open/decode/close)\n");

#ifdef _WIN32
    /* WIC is COM; init once for the whole multi-run session. */
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
        com_inited = 1;
#endif

    for (r = 0; r < RUNS; r++) {
        if (bench_ours_session(data, len, &ours[r]) != 0) {
            ours[r].ok = 0;
            ours[r].open_ms = ours[r].decode_ms = ours[r].close_ms = ours[r].total_ms =
                -1.0;
        }

#ifdef HEIC_HAVE_LIBHEIF
        if (heic_bench_libheif_session(data, len, &libraw[r]) != 0) {
            libraw[r].ok = 0;
            libraw[r].open_ms = libraw[r].decode_ms = libraw[r].close_ms =
                libraw[r].total_ms = -1.0;
        }
        lib[r].open_ms = libraw[r].open_ms;
        lib[r].decode_ms = libraw[r].decode_ms;
        lib[r].close_ms = libraw[r].close_ms;
        lib[r].total_ms = libraw[r].total_ms;
        lib[r].width = libraw[r].width;
        lib[r].height = libraw[r].height;
        lib[r].ok = libraw[r].ok;
#endif
#ifdef _WIN32
        if (!com_inited || heic_bench_wic_session(data, len, &wic[r]) != 0) {
            if (!com_inited) {
                wic[r].ok = 0;
                wic[r].total_ms = -1.0;
                wic[r].width = wic[r].height = 0;
                snprintf(wic[r].error, sizeof wic[r].error, "CoInitializeEx failed");
            } else {
                wic[r].ok = 0;
                if (wic[r].total_ms < 0.0) wic[r].total_ms = -1.0;
            }
        }
#endif
    }

#ifdef HEIC_HAVE_LIBHEIF
    /* Both sides failed every run → unsupported (unci, etc.); skip quietly. */
    if (!(ours[0].ok || ours[1].ok || ours[2].ok) &&
        !(lib[0].ok || lib[1].ok || lib[2].ok)) {
        const char *lib_error = libraw[0].error[0] ? libraw[0].error
                                : libraw[1].error[0] ? libraw[1].error
                                : libraw[2].error[0] ? libraw[2].error
                                                    : "unknown error";
        printf("BENCH_RESULT ours_ok=0 libheif_ok=0 "
               "ours_open=-1 ours_decode=-1 ours_close=-1 ours_total=-1 "
               "libheif_open=-1 libheif_decode=-1 libheif_close=-1 "
               "libheif_total=-1"
#ifdef _WIN32
               " wic_ok=0 wic_total=-1"
#endif
               "\n");
        printf("BENCH_LIBHEIF_ERROR %s\n", lib_error);
        printf("skip: both heic and libheif failed (unsupported / not decodable)\n");
#ifdef _WIN32
        if (com_inited) CoUninitialize();
#endif
        return 0;
    }
#endif

    for (r = 0; r < RUNS; r++) {
        if (!ours[r].ok)
            fprintf(stderr, "bench: heic session run %d failed\n", r + 1);
        bench_print_session_line("heic", &ours[r]);
#ifdef HEIC_HAVE_LIBHEIF
        /* libheif-only failures are expected for some Nokia conformance files;
         * do not spam stderr (table shows ERROR; note below explains). */
        bench_print_session_line("libheif", &lib[r]);
#endif
#ifdef _WIN32
        if (wic[r].ok)
            printf("wic total: %.2f\n", wic[r].total_ms);
        else
            printf("wic total: ERROR\n");
#endif
    }

#ifndef HEIC_HAVE_LIBHEIF
    fprintf(stderr,
            "bench: built without libheif (rebuild with bun cmd/build.ts -libheif)\n");
    {
        double bo = bench_best3(ours[0].open_ms, ours[1].open_ms, ours[2].open_ms);
        double bd = bench_best3(ours[0].decode_ms, ours[1].decode_ms, ours[2].decode_ms);
        double bc = bench_best3(ours[0].close_ms, ours[1].close_ms, ours[2].close_ms);
        double bt = bench_best3(ours[0].total_ms, ours[1].total_ms, ours[2].total_ms);
        printf("BENCH heic open=%.4f decode=%.4f close=%.4f total=%.4f size=%ux%u ok=%d\n",
               bo, bd, bc, bt,
               (unsigned)(ours[0].width ? ours[0].width
                          : ours[1].width ? ours[1].width : ours[2].width),
               (unsigned)(ours[0].height ? ours[0].height
                          : ours[1].height ? ours[1].height : ours[2].height),
               ours[0].ok || ours[1].ok || ours[2].ok);
    }
#ifdef _WIN32
    if (com_inited) CoUninitialize();
#endif
    return (ours[0].ok || ours[1].ok || ours[2].ok) ? 0 : 1;
#else
    {
        bench_cmp_row rows[4];
        double o_open = bench_best3(ours[0].open_ms, ours[1].open_ms, ours[2].open_ms);
        double o_dec = bench_best3(ours[0].decode_ms, ours[1].decode_ms, ours[2].decode_ms);
        double o_close = bench_best3(ours[0].close_ms, ours[1].close_ms, ours[2].close_ms);
        double o_tot = bench_best3(ours[0].total_ms, ours[1].total_ms, ours[2].total_ms);
        double l_open = bench_best3(lib[0].open_ms, lib[1].open_ms, lib[2].open_ms);
        double l_dec = bench_best3(lib[0].decode_ms, lib[1].decode_ms, lib[2].decode_ms);
        double l_close = bench_best3(lib[0].close_ms, lib[1].close_ms, lib[2].close_ms);
        double l_tot = bench_best3(lib[0].total_ms, lib[1].total_ms, lib[2].total_ms);
#ifdef _WIN32
        double w_tot = bench_best3(wic[0].total_ms, wic[1].total_ms, wic[2].total_ms);
        int wic_ok = wic[0].ok || wic[1].ok || wic[2].ok;
        const char *wic_error = wic[0].error[0] ? wic[0].error
                                : wic[1].error[0] ? wic[1].error
                                : wic[2].error[0] ? wic[2].error
                                                  : "unknown error";
#endif

        strcpy(rows[0].op, "open");
        rows[0].ours = o_open;
        rows[0].lib = l_open;
        strcpy(rows[1].op, "decode");
        rows[1].ours = o_dec;
        rows[1].lib = l_dec;
        strcpy(rows[2].op, "close");
        rows[2].ours = o_close;
        rows[2].lib = l_close;
        strcpy(rows[3].op, "total");
        rows[3].ours = o_tot;
        rows[3].lib = l_tot;

        {
            int ours_ok = ours[0].ok || ours[1].ok || ours[2].ok;
            int lib_ok = lib[0].ok || lib[1].ok || lib[2].ok;
            const char *lib_error = libraw[0].error[0] ? libraw[0].error
                                    : libraw[1].error[0] ? libraw[1].error
                                    : libraw[2].error[0] ? libraw[2].error
                                                        : "unknown error";
            int ret;

            printf("(best of %d runs; + = heic slower)\n", RUNS);
            bench_print_compare_table(rows, 4);
#ifdef _WIN32
            if (wic_ok)
                printf("wic total (best of %d): %.2f ms\n", RUNS, w_tot);
            else
                printf("wic: SKIP (%s)\n", wic_error);
#endif
            printf("BENCH_RESULT ours_ok=%d libheif_ok=%d "
                   "ours_open=%.4f ours_decode=%.4f ours_close=%.4f ours_total=%.4f "
                   "libheif_open=%.4f libheif_decode=%.4f libheif_close=%.4f "
                   "libheif_total=%.4f"
#ifdef _WIN32
                   " wic_ok=%d wic_total=%.4f"
#endif
                   "\n",
                   ours_ok, lib_ok, o_open, o_dec, o_close, o_tot,
                   l_open, l_dec, l_close, l_tot
#ifdef _WIN32
                   ,
                   wic_ok, wic_ok ? w_tot : -1.0
#endif
            );
            if (!lib_ok) printf("BENCH_LIBHEIF_ERROR %s\n", lib_error);
#ifdef _WIN32
            if (!wic_ok) printf("BENCH_WIC_ERROR %s\n", wic_error);
#endif

            if (ours_ok && lib_ok) {
                uint32_t ow = ours[0].width ? ours[0].width
                              : ours[1].width ? ours[1].width : ours[2].width;
                uint32_t oh = ours[0].height ? ours[0].height
                              : ours[1].height ? ours[1].height : ours[2].height;
                uint32_t lw = lib[0].width ? lib[0].width
                              : lib[1].width ? lib[1].width : lib[2].width;
                uint32_t lh = lib[0].height ? lib[0].height
                              : lib[1].height ? lib[1].height : lib[2].height;
                if (ow != lw || oh != lh)
                    printf("note: size heic=%ux%u libheif=%ux%u\n", (unsigned)ow,
                           (unsigned)oh, (unsigned)lw, (unsigned)lh);
                ret = 0;
            } else if (ours_ok && !lib_ok) {
                /* Oracle reject (e.g. Nokia C021 double dimg iref) while we
                 * decode primary: report heic timings, exit 0 so bench.ts skips
                 * compare rather than failing the run. */
                printf("note: libheif failed; heic timings only (no compare)\n");
                ret = 0;
            } else if (!ours_ok && lib_ok) {
                printf("note: heic failed; libheif timings only\n");
                ret = 1;
            } else {
                ret = 1;
            }
#ifdef _WIN32
            if (com_inited) CoUninitialize();
#endif
            return ret;
        }
    }
#endif
}

#ifdef HEIC_HAVE_LIBHEIF
/* Compare RGB pixels against libheif; print mse / maxdiff.
 * Exit: 0 both ok (+ mse line), 1 heic fail libheif ok, 2 size mismatch,
 *       3 both failed, 4 libheif fail heic ok. */
static int verify_extras(heic_ctx *ctx, heic_doc *doc, const heic_image *img,
                         const uint8_t *data, size_t len);

static int do_verify(const uint8_t *data, size_t len)
{
    heic_ctx *ctx = NULL;
    heic_doc *doc = NULL;
    heic_image *img = NULL;
    uint8_t *ref = NULL;
    int rw = 0, rh = 0, rstride = 0;
    int lib_ok = 0, heic_ok = 0;
    int x, y;
    double sse = 0.0;
    int maxd = 0, ndiff = 0;
    size_t n;
    int rc = 3;

    lib_ok = heic_libheif_decode_rgb(data, len, &ref, &rw, &rh, &rstride) == 0 && ref != NULL;

    ctx = heic_ctx_new(NULL, NULL, on_error_quiet, NULL);
    if (ctx) {
        doc = heic_doc_open(ctx, data, len);
        if (doc) {
            img = heic_doc_decode(doc, HEIC_FORMAT_RGB);
            if (img) heic_ok = 1;
        }
    }

    if (!lib_ok && !heic_ok) {
        printf("verify: both failed (unsupported / not decodable)\n");
        rc = 3;
        goto done;
    }
    if (!lib_ok && heic_ok) {
        printf("verify: libheif failed; heic ok size=%ux%u\n", (unsigned)img->width,
               (unsigned)img->height);
        rc = 4;
        goto done;
    }
    if (lib_ok && !heic_ok) {
        printf("verify: heic failed; libheif ok size=%dx%d\n", rw, rh);
        rc = 1;
        goto done;
    }

    if ((int)img->width != rw || (int)img->height != rh) {
        printf("size mismatch heic=%ux%u libheif=%dx%d\n", (unsigned)img->width,
               (unsigned)img->height, rw, rh);
        rc = 2;
        goto done;
    }

    for (y = 0; y < rh; y++) {
        const uint8_t *a = img->data + (size_t)y * (size_t)img->stride;
        const uint8_t *b = ref + (size_t)y * (size_t)rstride;
        for (x = 0; x < rw * 3; x++) {
            int d = (int)a[x] - (int)b[x];
            if (d < 0) d = -d;
            if (d) {
                ndiff++;
                sse += (double)d * (double)d;
                if (d > maxd) maxd = d;
            }
        }
    }
    n = (size_t)rw * (size_t)rh * 3u;
    printf("%dx%d mse=%.4f maxdiff=%d n_diff=%d", rw, rh, sse / (double)n, maxd, ndiff);
    /* rc 5: the RGB bitmap is comparable but another API result is wrong. */
    rc = verify_extras(ctx, doc, img, data, len) == 0 ? 0 : 5;
    printf("\n");

done:
    if (img) heic_image_destroy(ctx, img);
    if (doc) heic_doc_close(doc);
    if (ctx) heic_ctx_free(ctx);
    free(ref);
    return rc;
}

/* mse / maxdiff of one channel between two packed 8-bit images. */
static double channel_mse(const uint8_t *a, int a_stride, const uint8_t *b,
                          int b_stride, int w, int h, int bpp, int first,
                          int count, int *maxd)
{
    double sse = 0.0;
    int x, y, c;
    *maxd = 0;
    for (y = 0; y < h; y++) {
        const uint8_t *ra = a + (size_t)y * (size_t)a_stride;
        const uint8_t *rb = b + (size_t)y * (size_t)b_stride;
        for (x = 0; x < w; x++)
            for (c = first; c < first + count; c++) {
                int d = (int)ra[x * bpp + c] - (int)rb[x * bpp + c];
                if (d < 0) d = -d;
                if (d > *maxd) *maxd = d;
                sse += (double)d * (double)d;
            }
    }
    return sse / ((double)w * (double)h * (double)count);
}

static int meta_equal(const char *what, int ours_ok, const uint8_t *ours,
                      size_t ours_len, int ref_ok, const uint8_t *ref,
                      size_t ref_len)
{
    if (ours_ok != ref_ok) {
        printf(" %s_presence(heic=%d,libheif=%d)", what, ours_ok, ref_ok);
        return 0;
    }
    if (ours_ok && (ours_len != ref_len || memcmp(ours, ref, ref_len) != 0)) {
        printf(" %s_differs(heic=%zu,libheif=%zu)", what, ours_len, ref_len);
        return 0;
    }
    return 1;
}

/* Everything -verify checks besides the primary RGB bitmap. Prints extra
   tokens on the current output line; returns 0 when all of it agrees with
   libheif (pixel thresholds for alpha_mse / thumb_mse are applied by
   tests.ts), 1 on a hard mismatch. */
static int verify_extras(heic_ctx *ctx, heic_doc *doc, const heic_image *img,
                         const uint8_t *data, size_t len)
{
    heic_libheif_extras ref;
    heic_image_info info;
    int ok = 1;

    /* heic_doc_decode_into with a padded stride must give the same pixels as
       heic_doc_decode and must not write into the padding. */
    {
        int stride = (int)img->width * 3 + 7;
        size_t cap = (size_t)stride * img->height;
        uint8_t *buf = (uint8_t *)malloc(cap);
        uint32_t y;
        int same = buf != NULL;
        if (buf) {
            memset(buf, 0xA5, cap);
            same = heic_doc_decode_into(doc, HEIC_FORMAT_RGB, buf, cap,
                                        stride) == 0;
            for (y = 0; same && y < img->height; y++) {
                const uint8_t *row = buf + (size_t)y * (size_t)stride;
                int k;
                if (memcmp(row, img->data + (size_t)y * (size_t)img->stride,
                           (size_t)img->width * 3) != 0)
                    same = 0;
                for (k = 0; k < 7; k++)
                    if (row[img->width * 3 + k] != 0xA5) same = 0;
            }
            free(buf);
        }
        if (same && heic_doc_output_size(doc, HEIC_FORMAT_RGB)
                        != (size_t)img->width * img->height * 3)
            same = 0;
        if (!same) {
            printf(" decode_into_mismatch");
            ok = 0;
        }
    }

    /* BGRA (the SumatraPDF format) and BGR take a different conversion path
       from RGB; the colour bytes must be the same ones, reordered. */
    {
        heic_image *bgra = heic_doc_decode(doc, HEIC_FORMAT_BGRA);
        heic_image *bgr = heic_doc_decode(doc, HEIC_FORMAT_BGR);
        int same = bgra && bgr && bgra->width == img->width
                   && bgra->height == img->height && bgr->width == img->width
                   && bgr->height == img->height;
        uint32_t x, y;
        for (y = 0; same && y < img->height; y++) {
            const uint8_t *a = img->data + (size_t)y * (size_t)img->stride;
            const uint8_t *b = bgra->data + (size_t)y * (size_t)bgra->stride;
            const uint8_t *c = bgr->data + (size_t)y * (size_t)bgr->stride;
            for (x = 0; x < img->width; x++) {
                if (a[x * 3] != b[x * 4 + 2] || a[x * 3 + 1] != b[x * 4 + 1]
                    || a[x * 3 + 2] != b[x * 4]
                    || a[x * 3] != c[x * 3 + 2] || a[x * 3 + 1] != c[x * 3 + 1]
                    || a[x * 3 + 2] != c[x * 3]) {
                    same = 0;
                    break;
                }
            }
        }
        if (bgra) heic_image_destroy(ctx, bgra);
        if (bgr) heic_image_destroy(ctx, bgr);
        if (!same) {
            printf(" bgr_bgra_mismatch");
            ok = 0;
        }
    }

    if (heic_doc_info(doc, &info) != 0) {
        printf(" info_failed");
        return 1;
    }
    if (heic_libheif_extras_load(data, len, &ref) != 0) {
        printf(" extras_oracle_failed");
        return 1;
    }

    if (info.has_alpha != ref.has_alpha) {
        printf(" alpha_presence(heic=%d,libheif=%d)", info.has_alpha,
               ref.has_alpha);
        ok = 0;
    } else if (ref.has_alpha && ref.rgba) {
        heic_image *rgba = heic_doc_decode(doc, HEIC_FORMAT_RGBA);
        if (!rgba || (int)rgba->width != ref.w || (int)rgba->height != ref.h) {
            printf(" alpha_decode_mismatch");
            ok = 0;
        } else {
            int maxd;
            double mse = channel_mse(rgba->data, rgba->stride, ref.rgba,
                                     ref.w * 4, ref.w, ref.h, 4, 3, 1, &maxd);
            printf(" alpha_mse=%.4f alpha_maxdiff=%d", mse, maxd);
        }
        if (rgba) heic_image_destroy(ctx, rgba);
    }

    if (info.has_thumbnail != ref.has_thumb) {
        printf(" thumb_presence(heic=%d,libheif=%d)", info.has_thumbnail,
               ref.has_thumb);
        ok = 0;
    } else if (ref.has_thumb && ref.thumb_rgb) {
        heic_image *th = heic_doc_decode_thumbnail(doc, HEIC_FORMAT_RGB);
        if (!th || (int)th->width != ref.thumb_w
            || (int)th->height != ref.thumb_h) {
            printf(" thumb_decode_mismatch(heic=%ux%u,libheif=%dx%d)",
                   th ? (unsigned)th->width : 0, th ? (unsigned)th->height : 0,
                   ref.thumb_w, ref.thumb_h);
            ok = 0;
        } else {
            int maxd;
            double mse = channel_mse(th->data, th->stride, ref.thumb_rgb,
                                     ref.thumb_w * 3, ref.thumb_w, ref.thumb_h,
                                     3, 0, 3, &maxd);
            printf(" thumb_mse=%.4f thumb_maxdiff=%d", mse, maxd);
        }
        if (th) heic_image_destroy(ctx, th);
    }

    {
        uint8_t *m = NULL;
        size_t n = 0;
        int got;
        /* libheif hands out the raw Exif item; ours skips the 4-byte offset
           header plus the offset. A raw-TIFF item (no valid header) is a
           documented case where heic_doc_exif returns nothing. */
        const uint8_t *want = NULL;
        size_t want_len = 0;
        if (ref.exif && ref.exif_len >= 4) {
            size_t off = ((size_t)ref.exif[0] << 24) | ((size_t)ref.exif[1] << 16)
                         | ((size_t)ref.exif[2] << 8) | ref.exif[3];
            if (off <= ref.exif_len - 4) {
                want = ref.exif + 4 + off;
                want_len = ref.exif_len - 4 - off;
            }
        }
        got = heic_doc_exif(doc, &m, &n);
        if (info.has_exif != ref.has_exif) {
            printf(" exif_presence(heic=%d,libheif=%d)", info.has_exif,
                   ref.has_exif);
            ok = 0;
        } else if (!meta_equal("exif", got, m, n, want != NULL, want, want_len))
            ok = 0;
        if (got) heic_free(ctx, m);

        m = NULL;
        n = 0;
        got = heic_doc_xmp(doc, &m, &n);
        if (!meta_equal("xmp", got, m, n, ref.xmp != NULL, ref.xmp, ref.xmp_len))
            ok = 0;
        if (got) heic_free(ctx, m);

        m = NULL;
        n = 0;
        got = heic_doc_icc(doc, &m, &n);
        if (!meta_equal("icc", got, m, n, ref.icc != NULL, ref.icc, ref.icc_len))
            ok = 0;
        if (got) heic_free(ctx, m);
    }

    heic_libheif_extras_free(&ref);
    return ok ? 0 : 1;
}

static int do_verify_gain_map(const uint8_t *data, size_t len)
{
    heic_ctx *ctx = NULL;
    heic_doc *doc = NULL;
    heic_image *img = NULL;
    uint8_t *ref = NULL;
    int rw = 0, rh = 0, rstride = 0;
    int x, y, maxd = 0, rc = 1;
    uint64_t ndiff = 0;
    double sse = 0.0;
    char error[256];

    if (heic_libheif_decode_gain_map_rgb(
            data, len, &ref, &rw, &rh, &rstride,
            error, sizeof error) != 0) {
        printf("gain-map oracle failed: %s\n", error);
        goto done;
    }
    ctx = heic_ctx_new(NULL, NULL, on_error_quiet, NULL);
    if (!ctx) goto done;
    doc = heic_doc_open(ctx, data, len);
    if (!doc) goto done;
    img = heic_doc_decode_gain_map(doc, HEIC_FORMAT_RGB);
    if (!img) goto done;
    if ((int)img->width != rw || (int)img->height != rh) {
        printf("gain-map size mismatch heic=%ux%u libheif=%dx%d\n",
               (unsigned)img->width, (unsigned)img->height, rw, rh);
        goto done;
    }
    for (y = 0; y < rh; y++) {
        const uint8_t *a =
            img->data + (size_t)y * (size_t)img->stride;
        const uint8_t *b =
            ref + (size_t)y * (size_t)rstride;
        for (x = 0; x < rw * 3; x++) {
            int d = (int)a[x] - (int)b[x];
            if (d < 0) d = -d;
            if (d) {
                ndiff++;
                sse += (double)d * (double)d;
                if (d > maxd) maxd = d;
            }
        }
    }
    printf("gain-map %dx%d mse=%.4f maxdiff=%d n_diff=%llu\n",
           rw, rh, sse / ((double)rw * (double)rh * 3.0), maxd,
           (unsigned long long)ndiff);
    rc = 0;
done:
    if (img) heic_image_destroy(ctx, img);
    if (doc) heic_doc_close(doc);
    if (ctx) heic_ctx_free(ctx);
    free(ref);
    return rc;
}

static uint32_t sequence_presentation_rank(const heic_sequence *seq,
                                           uint32_t sample)
{
    uint32_t i, rank = 0;
    int64_t pts = seq->samples[sample].composition_time;
    for (i = 0; i < seq->sample_count; i++)
        if (seq->samples[i].composition_time < pts
            || (seq->samples[i].composition_time == pts && i < sample))
            rank++;
    return rank;
}

static int compare_sequence_frame(
    heic_ctx *ctx, heic_sequence_decoder *decoder,
    const heic_sequence *seq, uint32_t frame,
    const uint8_t *ref, uint32_t ref_frames, int rw, int rh, int rstride,
    double *sse, uint64_t *compared, uint64_t *ndiff, int *maxd)
{
    uint32_t sample, rank;
    heic_image *img;
    int y, x;
    if (frame >= seq->frame_count) return -1;
    sample = seq->frame_samples[frame];
    if (sample >= seq->sample_count) return -1;
    rank = sequence_presentation_rank(seq, sample);
    if (rank >= ref_frames) return -1;
    img = heic_sequence_decoder_decode_frame(decoder, frame);
    if (!img) return -1;
    if ((int)img->width != rw || (int)img->height != rh) {
        heic_image_destroy(ctx, img);
        return -1;
    }
    for (y = 0; y < rh; y++) {
        const uint8_t *a =
            img->data + (size_t)y * (size_t)img->stride;
        const uint8_t *b =
            ref + (size_t)rank * (size_t)rstride * (size_t)rh
                + (size_t)y * (size_t)rstride;
        for (x = 0; x < rw * 4; x++) {
            int d = (int)a[x] - (int)b[x];
            if (d < 0) d = -d;
            if (d) {
                (*ndiff)++;
                *sse += (double)d * (double)d;
                if (d > *maxd) *maxd = d;
            }
        }
    }
    *compared += (uint64_t)rw * (uint64_t)rh * 4u;
    heic_image_destroy(ctx, img);
    return 0;
}

static int do_verify_sequence(const uint8_t *data, size_t len)
{
    heic_ctx *ctx = NULL;
    heic_doc *doc = NULL;
    heic_sequence_decoder *decoder = NULL;
    heic_sequence_info info;
    uint8_t *ref = NULL;
    uint32_t ref_frames = 0, frame;
    int dependent = 0;
    int separate_primary = 0;
    int alpha = 0;
    int alpha_nonopaque = 0;
    int rw = 0, rh = 0, rstride = 0;
    double sse = 0.0;
    uint64_t compared = 0, ndiff = 0;
    int maxd = 0, rc = 1;
    char error[256];

    if (heic_libheif_decode_sequence_rgba(
            data, len, &ref, &ref_frames, &rw, &rh, &rstride,
            error, sizeof error) != 0) {
        printf("sequence oracle failed: %s\n", error);
        goto done;
    }
    ctx = heic_ctx_new(NULL, NULL, on_error_quiet, NULL);
    if (!ctx) goto done;
    doc = heic_doc_open(ctx, data, len);
    if (!doc || heic_doc_sequence_info(doc, &info) != 0
        || !doc->container.sequence)
        goto done;
    separate_primary =
        doc->container.sequence->coded_item_id
            != doc->container.primary_item_id;
    alpha = doc->container.sequence->alpha != NULL;
    decoder = heic_sequence_decoder_new(doc, HEIC_FORMAT_RGBA);
    if (!decoder) goto done;
    for (frame = 0; frame < info.frame_count; frame++) {
        const heic_sequence *seq = doc->container.sequence;
        if (compare_sequence_frame(
                ctx, decoder, seq, frame, ref, ref_frames,
                rw, rh, rstride, &sse, &compared, &ndiff, &maxd) != 0)
            goto done;
    }
    if (alpha) {
        size_t i, n = (size_t)rw * (size_t)rh * 4;
        for (i = 3; i < n; i += 4)
            if (ref[i] != 255) {
                alpha_nonopaque = 1;
                break;
            }
    }
    {
        const heic_sequence *seq = doc->container.sequence;
        uint32_t seek_order[3];
        seek_order[0] = info.frame_count - 1;
        seek_order[1] = info.frame_count / 2;
        seek_order[2] = 0;
        heic_sequence_decoder_reset(decoder);
        for (frame = 0; frame < 3; frame++) {
            if (compare_sequence_frame(
                    ctx, decoder, seq, seek_order[frame], ref, ref_frames,
                    rw, rh, rstride, &sse, &compared, &ndiff, &maxd) != 0)
                goto done;
            if (frame + 1 < 3) heic_sequence_decoder_reset(decoder);
        }
        /* The one-shot API must give exactly what the stateful decoder
           gives for the same frame. */
        for (frame = 0; frame < 3; frame++) {
            heic_image *one = heic_doc_decode_sequence_frame(
                doc, seek_order[frame], HEIC_FORMAT_RGBA);
            heic_image *st;
            int same;
            heic_sequence_decoder_reset(decoder);
            st = heic_sequence_decoder_decode_frame(decoder, seek_order[frame]);
            same = one && st && one->width == st->width
                   && one->height == st->height && one->stride == st->stride
                   && memcmp(one->data, st->data,
                             (size_t)one->stride * one->height) == 0;
            if (one) heic_image_destroy(ctx, one);
            if (st) heic_image_destroy(ctx, st);
            if (!same) {
                printf("one-shot sequence frame %u differs from stateful decode\n",
                       (unsigned)seek_order[frame]);
                goto done;
            }
        }
    }
    {
        const heic_sequence *seq = doc->container.sequence;
        heic_item item;
        uint32_t sample;
        if (heic_container_get_item(
                &doc->container, seq->coded_item_id, &item) == 0
            && item.av1c) {
            for (sample = 1; sample < seq->sample_count; sample++) {
                heic_frame standalone;
                if (seq->samples[sample].is_sync) continue;
                memset(&standalone, 0, sizeof(standalone));
                if (heic_av1_decode(
                        ctx, item.av1c,
                        data + seq->samples[sample].offset,
                        seq->samples[sample].size,
                        &standalone, NULL) != 0) {
                    dependent = 1;
                } else {
                    heic_frame_free(ctx, &standalone);
                }
                break;
            }
        }
    }
    if (!compared) goto done;
    printf("sequence %u frames %dx%d mse=%.4f maxdiff=%d n_diff=%llu"
           " dependent=%d separate_primary=%d alpha=%d"
           " alpha_nonopaque=%d\n",
           (unsigned)info.frame_count, rw, rh, sse / (double)compared,
           maxd, (unsigned long long)ndiff, dependent, separate_primary,
           alpha, alpha_nonopaque);
    rc = 0;
done:
    heic_sequence_decoder_destroy(decoder);
    if (doc) heic_doc_close(doc);
    if (ctx) heic_ctx_free(ctx);
    free(ref);
    return rc;
}
#endif

/* Loop open/decode/close N times with winperf section marks around decode.
 * One heic_ctx is reused so cached backends (dav1d) stay warm across loops. */
static int do_profile_heic(const uint8_t *data, size_t len, int loops,
                           heic_format fmt)
{
    int i;
    double t0, total = 0.0;
    uint32_t w = 0, h = 0;
    heic_ctx *ctx;

    if (loops < 1) loops = 1;
    ctx = heic_ctx_new(NULL, NULL, on_error_quiet, NULL);
    if (!ctx) return 1;
    for (i = 0; i < loops; i++) {
        heic_doc *doc;
        heic_image *img;

        doc = heic_doc_open(ctx, data, len);
        if (!doc) {
            heic_ctx_free(ctx);
            return 1;
        }
        winperf_profile_start();
        t0 = bench_now_ms();
        img = heic_doc_decode(doc, fmt);
        total += bench_now_ms() - t0;
        winperf_profile_stop();
        if (!img) {
            heic_doc_close(doc);
            heic_ctx_free(ctx);
            return 1;
        }
        w = img->width;
        h = img->height;
        heic_image_destroy(ctx, img);
        heic_doc_close(doc);
    }
    heic_ctx_free(ctx);
    printf("profile-heic loops=%d size=%ux%u total_decode_ms=%.2f avg_ms=%.2f\n", loops,
           (unsigned)w, (unsigned)h, total, total / (double)loops);
    return 0;
}

#ifdef HEIC_HAVE_LIBHEIF
static int do_profile_libheif(const uint8_t *data, size_t len, int loops)
{
    int i;
    double total = 0.0;
    uint32_t w = 0, h = 0;

    if (loops < 1) loops = 1;
    for (i = 0; i < loops; i++) {
        heic_bench_session s;
        memset(&s, 0, sizeof(s));
        /* Mark whole session; open/close are tiny vs decode on big stills. */
        winperf_profile_start();
        if (heic_bench_libheif_session(data, len, &s) != 0 || !s.ok) {
            winperf_profile_stop();
            return 1;
        }
        winperf_profile_stop();
        total += s.decode_ms;
        w = s.width;
        h = s.height;
    }
    printf("profile-libheif loops=%d size=%ux%u total_decode_ms=%.2f avg_ms=%.2f\n", loops,
           (unsigned)w, (unsigned)h, total, total / (double)loops);
    return 0;
}
#endif

/* Tracking allocator for public-API tests. */
typedef struct {
    size_t live;
    size_t peak;
    int n_alloc;
    int n_free;
    int fail_after; /* if >=0, fail the Nth alloc (0-based after n_alloc++) */
} track_alloc;

static void *track_alloc_cb(void *user, void *ctx, size_t size)
{
    track_alloc *t = (track_alloc *)user;
    void *p;
    (void)ctx;
    if (t->fail_after >= 0 && t->n_alloc >= t->fail_after) {
        t->n_alloc++;
        return NULL;
    }
    p = malloc(size ? size : 1);
    if (!p) return NULL;
    t->n_alloc++;
    t->live += size;
    if (t->live > t->peak) t->peak = t->live;
    return p;
}

static void track_free_cb(void *user, void *ctx, void *p)
{
    track_alloc *t = (track_alloc *)user;
    (void)ctx;
    if (!p) return;
    t->n_free++;
    free(p);
}

/* SumatraPDF AvifReader surface: size probe, BGRA decode, EXIF TIFF blob. */
static int do_sumatra_surface_test(const uint8_t *data, size_t len)
{
    heic_ctx *ctx;
    heic_doc *doc;
    heic_image_info info;
    heic_image *img = NULL;
    uint8_t *exif = NULL;
    size_t elen = 0;
    int ok = 1;
    uint32_t x, y;
    size_t nonblack = 0;

    heic_init();
    ctx = heic_ctx_new(NULL, NULL, on_error_only, NULL);
    if (!ctx) {
        fprintf(stderr, "sumatra: ctx_new failed\n");
        return 1;
    }
    doc = heic_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "sumatra: doc_open failed\n");
        heic_ctx_free(ctx);
        return 1;
    }
    memset(&info, 0, sizeof(info));
    if (heic_doc_info(doc, &info) != 0 || info.width == 0 || info.height == 0) {
        fprintf(stderr, "sumatra: info failed\n");
        ok = 0;
        goto done;
    }
    /* Match AvifReader.cpp: BGRA primary decode. */
    img = heic_doc_decode(doc, HEIC_FORMAT_BGRA);
    if (!img || !img->data || img->width != info.width || img->height != info.height) {
        fprintf(stderr, "sumatra: BGRA decode failed\n");
        ok = 0;
        goto done;
    }
    if (img->format != HEIC_FORMAT_BGRA || img->stride < (int)info.width * 4) {
        fprintf(stderr, "sumatra: bad BGRA layout\n");
        ok = 0;
        goto done;
    }
    /* Sample a few pixels so we did not get an all-zero buffer. */
    for (y = 0; y < info.height; y += info.height > 8 ? info.height / 8 : 1) {
        const uint8_t *row = img->data + (size_t)y * (size_t)img->stride;
        for (x = 0; x < info.width; x += info.width > 8 ? info.width / 8 : 1) {
            const uint8_t *p = row + (size_t)x * 4;
            if (p[0] | p[1] | p[2]) nonblack++;
        }
    }
    if (nonblack == 0) {
        fprintf(stderr, "sumatra: BGRA output looks empty\n");
        ok = 0;
        goto done;
    }
    /* EXIF is optional; when present, prefix is already stripped (TIFF).
       Some HEIF EXIF payloads are non-TIFF (app-specific); only require
       TIFF magic when the blob is large enough to be a real TIFF. */
    if (heic_doc_exif(doc, &exif, &elen) && exif && elen >= 8) {
        int le = exif[0] == 'I' && exif[1] == 'I';
        int be = exif[0] == 'M' && exif[1] == 'M';
        if (!le && !be) {
            fprintf(stderr, "sumatra: EXIF does not look like TIFF (len=%zu)\n",
                    elen);
            ok = 0;
        }
        heic_free(ctx, exif);
        exif = NULL;
    } else if (exif) {
        heic_free(ctx, exif);
        exif = NULL;
    }
done:
    if (img) heic_image_destroy(ctx, img);
    heic_doc_close(doc);
    heic_ctx_free(ctx);
    if (ok)
        printf("sumatra: ok %ux%u bgra nonblack_samples=%zu\n",
               (unsigned)info.width, (unsigned)info.height, nonblack);
    return ok ? 0 : 1;
}

/* Custom allocator bookkeeping + cooperative abort. */
static int do_api_surface_test(const uint8_t *data, size_t len)
{
    track_alloc track;
    heic_ctx *ctx;
    heic_doc *doc;
    heic_image *img;
    heic_abort ab;
    int ok = 1;

    heic_init();
    memset(&track, 0, sizeof(track));
    track.fail_after = -1;
    ctx = heic_ctx_new(track_alloc_cb, track_free_cb, on_error_only, &track);
    if (!ctx) {
        fprintf(stderr, "api: tracked ctx_new failed\n");
        return 1;
    }
    doc = heic_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "api: doc_open failed\n");
        heic_ctx_free(ctx);
        return 1;
    }
    {
        /* heic_doc_output_size is the tight size within the limits, and 0
           for an image the limits forbid (decode must then fail too). */
        heic_image_info info;
        heic_limits narrow, back;
        if (heic_doc_info(doc, &info) != 0
            || heic_doc_output_size(doc, HEIC_FORMAT_BGRA)
                   != (size_t)info.width * info.height * 4) {
            fprintf(stderr, "api: output_size is not width*height*4\n");
            ok = 0;
        }
        memset(&narrow, 0, sizeof(narrow));
        narrow.max_width = info.width > 1 ? info.width - 1 : 1;
        heic_ctx_set_limits(ctx, &narrow);
        img = info.width > 1 ? heic_doc_decode(doc, HEIC_FORMAT_BGRA) : NULL;
        if (info.width > 1
            && (heic_doc_output_size(doc, HEIC_FORMAT_BGRA) != 0 || img)) {
            fprintf(stderr, "api: image wider than max_width was accepted\n");
            if (img) heic_image_destroy(ctx, img);
            ok = 0;
        }
        memset(&back, 0, sizeof(back));
        back.max_width = 16384;
        heic_ctx_set_limits(ctx, &back);
    }
    img = heic_doc_decode(doc, HEIC_FORMAT_BGR);
    if (!img || !img->data) {
        fprintf(stderr, "api: BGR decode failed\n");
        ok = 0;
    } else if (img->format != HEIC_FORMAT_BGR || img->stride < (int)img->width * 3) {
        fprintf(stderr, "api: bad BGR layout\n");
        ok = 0;
    }
    if (img) heic_image_destroy(ctx, img);
    /* Abort token: request before decode; decode must fail promptly. */
    heic_abort_init(&ab);
    heic_abort_request(&ab);
    img = heic_doc_decode_abortable(doc, HEIC_FORMAT_RGB, &ab);
    if (img) {
        fprintf(stderr, "api: expected abortable decode to fail\n");
        heic_image_destroy(ctx, img);
        ok = 0;
    }
    heic_doc_close(doc);
    heic_ctx_free(ctx);
    if (track.n_alloc == 0 || track.n_free == 0) {
        fprintf(stderr, "api: allocator not exercised (alloc=%d free=%d)\n",
                track.n_alloc, track.n_free);
        ok = 0;
    }
    /* Everything is closed: every allocation must have been freed once. */
    if (track.n_free != track.n_alloc) {
        fprintf(stderr, "api: free count %d != alloc %d (leak or double free)\n",
                track.n_free, track.n_alloc);
        ok = 0;
    }
    if (ok)
        printf("api: ok alloc=%d free=%d peak_bytes≈%zu\n",
               track.n_alloc, track.n_free, track.peak);
    return ok ? 0 : 1;
}

/* Exercise max_memory_bytes: a tiny cap must reject a frame alloc; freeing
   must allow a subsequent alloc of the same size. */
static int do_memory_limit_test(void)
{
    heic_ctx *ctx;
    heic_limits lim;
    heic_frame f;
    int ok = 1;

    heic_init();
    ctx = heic_ctx_new(NULL, NULL, NULL, NULL);
    if (!ctx) {
        fprintf(stderr, "memory-limit: ctx_new failed\n");
        return 1;
    }
    memset(&lim, 0, sizeof(lim));
    /* 64x64 4:2:0 16-bit planes need ~12 KB of payload + headers; cap at 4 KB. */
    lim.max_memory_bytes = 4096;
    heic_ctx_set_limits(ctx, &lim);
    memset(&f, 0, sizeof(f));
    if (heic_frame_alloc(ctx, &f, 64, 64, 8, 1) == 0) {
        fprintf(stderr, "memory-limit: expected frame alloc to fail under 4 KiB cap\n");
        heic_frame_free(ctx, &f);
        ok = 0;
    }
    /* Raise the cap and allocate successfully, then free and confirm reuse. */
    lim.max_memory_bytes = 1024 * 1024;
    heic_ctx_set_limits(ctx, &lim);
    memset(&f, 0, sizeof(f));
    if (heic_frame_alloc(ctx, &f, 64, 64, 8, 1) != 0) {
        fprintf(stderr, "memory-limit: frame alloc failed under 1 MiB cap\n");
        ok = 0;
    } else {
        heic_frame_free(ctx, &f);
        memset(&f, 0, sizeof(f));
        if (heic_frame_alloc(ctx, &f, 64, 64, 8, 1) != 0) {
            fprintf(stderr, "memory-limit: realloc after free failed\n");
            ok = 0;
        } else {
            heic_frame_free(ctx, &f);
        }
    }
    /* Tiny successive zalloc until OOM under a small cap. */
    lim.max_memory_bytes = 1024;
    heic_ctx_set_limits(ctx, &lim);
    {
        void *blocks[64];
        void *again;
        int n = 0, i;
        memset(blocks, 0, sizeof(blocks));
        for (i = 0; i < 64; i++) {
            blocks[i] = heic_zalloc(ctx, 200);
            if (!blocks[i]) break;
            n++;
        }
        if (n == 0 || n == 64) {
            fprintf(stderr,
                    "memory-limit: expected partial zalloc under 1 KiB (got %d)\n",
                    n);
            ok = 0;
        }
        for (i = 0; i < n; i++) heic_free_buf(ctx, blocks[i]);
        again = heic_zalloc(ctx, 200);
        if (!again) {
            fprintf(stderr, "memory-limit: zalloc after free failed\n");
            ok = 0;
        } else {
            heic_free_buf(ctx, again);
        }
    }
    heic_ctx_free(ctx);
    if (ok) printf("memory-limit: ok\n");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *out_path = NULL;
    int do_info = 0, do_exif = 0, do_decode = 0, do_thumbnail = 0;
    int do_bench_mode = 0, do_verify_mode = 0;
    int do_verify_sequence_mode = 0, do_verify_gain_map_mode = 0;
    int do_hevc_sequence_mode = 0;
    int do_sequence_info = 0, sequence_frame = -1;
    int do_memory_limit = 0, do_sumatra = 0, do_api = 0;
    int profile_heic_loops = 0, profile_libheif_loops = 0;
    int hevc_sequence_frames = 1 << 20; /* whole stream unless -hevc-frames */
    int want_rgba = 0, want_bgra = 0;
    int i;
    uint8_t *data = NULL;
    size_t len = 0;
    heic_ctx *ctx;
    heic_doc *doc;
    int rc = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-info") == 0) do_info = 1;
        else if (strcmp(argv[i], "-exif") == 0) do_exif = 1;
        else if (strcmp(argv[i], "-thumbnail") == 0) do_thumbnail = 1;
        else if (strcmp(argv[i], "-rgba") == 0) want_rgba = 1;
        else if (strcmp(argv[i], "-bgra") == 0) want_bgra = 1;
        else if (strcmp(argv[i], "-bench") == 0) do_bench_mode = 1;
        else if (strcmp(argv[i], "-verify") == 0) do_verify_mode = 1;
        else if (strcmp(argv[i], "-verify-gain-map") == 0)
            do_verify_gain_map_mode = 1;
        else if (strcmp(argv[i], "-verify-sequence") == 0)
            do_verify_sequence_mode = 1;
        else if (strcmp(argv[i], "-memory-limit") == 0)
            do_memory_limit = 1;
        else if (strcmp(argv[i], "-sumatra") == 0)
            do_sumatra = 1;
        else if (strcmp(argv[i], "-api") == 0)
            do_api = 1;
        else if (strcmp(argv[i], "-sequence-info") == 0)
            do_sequence_info = 1;
        else if (strcmp(argv[i], "-sequence-frame") == 0 && i + 1 < argc) {
            sequence_frame = atoi(argv[++i]);
            if (sequence_frame < 0) sequence_frame = -1;
        }
        else if (strcmp(argv[i], "-hevc-sequence") == 0)
            do_hevc_sequence_mode = 1;
        else if (strcmp(argv[i], "-hevc-frames") == 0 && i + 1 < argc) {
            hevc_sequence_frames = atoi(argv[++i]);
            if (hevc_sequence_frames < 1) hevc_sequence_frames = 1;
        }
        else if (strcmp(argv[i], "-profile-heic") == 0 && i + 1 < argc) {
            profile_heic_loops = atoi(argv[++i]);
            if (profile_heic_loops < 1) profile_heic_loops = 1;
        } else if (strcmp(argv[i], "-profile-libheif") == 0 && i + 1 < argc) {
            profile_libheif_loops = atoi(argv[++i]);
            if (profile_libheif_loops < 1) profile_libheif_loops = 1;
        } else if (strcmp(argv[i], "-out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
            do_decode = 1;
        } else if (argv[i][0] != '-')
            path = argv[i];
        else {
            /* Unknown flag, or a value flag with its value missing: a typo
               must not silently turn into a different (passing) run. */
            fprintf(stderr, "heic_test: bad option %s\n", argv[i]);
            return 2;
        }
    }
    if (do_memory_limit) {
        return do_memory_limit_test();
    }
    if (sequence_frame >= 0) do_decode = 0;
    if (!path ||
        (!do_info && !do_exif && !do_decode && !do_bench_mode
         && !do_verify_mode && !do_hevc_sequence_mode
         && !do_verify_gain_map_mode
         && !do_verify_sequence_mode
         && !do_sequence_info && sequence_frame < 0
         && !profile_heic_loops && !profile_libheif_loops
         && !do_sumatra && !do_api)) {
        fprintf(stderr,
                "usage: heic_test [-info] [-exif] [-thumbnail] [-rgba] [-bgra] [-bench] [-verify] "
                "[-verify-gain-map] [-verify-sequence] [-memory-limit] "
                "[-sumatra] [-api] "
                "[-sequence-info] [-sequence-frame N] "
                "[-hevc-sequence [-hevc-frames N]] "
                "[-profile-heic N] [-profile-libheif N] "
                "[-out out.ppm] file.heic\n");
        return 2;
    }

    data = read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    heic_init();

    if (do_sumatra) {
        rc = do_sumatra_surface_test(data, len);
        free(data);
        return rc;
    }
    if (do_api) {
        rc = do_api_surface_test(data, len);
        free(data);
        return rc;
    }

    if (do_hevc_sequence_mode) {
        rc = do_hevc_sequence(data, len, out_path, hevc_sequence_frames);
        free(data);
        return rc;
    }

    if (profile_heic_loops) {
        rc = do_profile_heic(data, len, profile_heic_loops,
                             want_bgra ? HEIC_FORMAT_BGRA
                             : want_rgba ? HEIC_FORMAT_RGBA : HEIC_FORMAT_RGB);
        free(data);
        return rc;
    }
#ifdef HEIC_HAVE_LIBHEIF
    if (profile_libheif_loops) {
        rc = do_profile_libheif(data, len, profile_libheif_loops);
        free(data);
        return rc;
    }
#else
    if (profile_libheif_loops) {
        fprintf(stderr, "profile-libheif: needs HEIC_HAVE_LIBHEIF\n");
        free(data);
        return 2;
    }
#endif

    if (do_bench_mode) {
        rc = do_bench(data, len);
        free(data);
        return rc;
    }

#ifdef HEIC_HAVE_LIBHEIF
    if (do_verify_gain_map_mode) {
        rc = do_verify_gain_map(data, len);
        free(data);
        return rc;
    }
    if (do_verify_sequence_mode) {
        rc = do_verify_sequence(data, len);
        free(data);
        return rc;
    }
    if (do_verify_mode) {
        rc = do_verify(data, len);
        free(data);
        return rc;
    }
#else
    if (do_verify_gain_map_mode) {
        fprintf(stderr, "verify-gain-map: needs HEIC_HAVE_LIBHEIF\n");
        free(data);
        return 2;
    }
    if (do_verify_sequence_mode) {
        fprintf(stderr, "verify-sequence: needs HEIC_HAVE_LIBHEIF\n");
        free(data);
        return 2;
    }
    if (do_verify_mode) {
        fprintf(stderr, "verify: needs HEIC_HAVE_LIBHEIF (build with -libheif)\n");
        free(data);
        return 2;
    }
#endif

    ctx = heic_ctx_new(NULL, NULL, on_error, NULL);
    if (!ctx) {
        free(data);
        return 1;
    }
    doc = heic_doc_open(ctx, data, len);
    if (!doc) {
        fprintf(stderr, "open failed: %s\n", path);
        heic_ctx_free(ctx);
        free(data);
        return 1;
    }

    if (do_info) {
        heic_image_info info;
        if (heic_doc_info(doc, &info) != 0) {
            fprintf(stderr, "info failed\n");
            goto done;
        }
        printf("%ux%u kind=%s bit_depth=%d alpha=%d gain_map=%d exif=%d xmp=%d thumb=%d\n",
               (unsigned)info.width, (unsigned)info.height, kind_str(heic_doc_kind(doc)),
               info.bit_depth, info.has_alpha, info.has_gain_map,
               info.has_exif, info.has_xmp,
               info.has_thumbnail);
        rc = 0;
    }

    if (do_sequence_info) {
        heic_sequence_info info;
        uint32_t frame;
        if (heic_doc_sequence_info(doc, &info) != 0) {
            fprintf(stderr, "sequence info failed\n");
            rc = 1; /* an earlier mode (-info) may have set rc = 0 */
            goto done;
        }
        printf("sequence frames=%u timescale=%u duration=%llu repetitions=%u\n",
               (unsigned)info.frame_count, (unsigned)info.timescale,
               (unsigned long long)info.duration,
               (unsigned)info.repetition_count);
        for (frame = 0; frame < info.frame_count; frame++) {
            heic_sequence_frame_info fi;
            if (heic_doc_sequence_frame_info(doc, frame, &fi) != 0) {
                fprintf(stderr, "sequence frame info failed at %u\n",
                        (unsigned)frame);
                rc = 1;
                goto done;
            }
            printf("frame %u time=%llu duration=%u sync=%d\n",
                   (unsigned)frame,
                   (unsigned long long)fi.presentation_time,
                   (unsigned)fi.duration, fi.is_sync);
        }
        rc = 0;
    }

    if (sequence_frame >= 0) {
        heic_format fmt = want_rgba ? HEIC_FORMAT_RGBA : HEIC_FORMAT_RGB;
        heic_image *img = heic_doc_decode_sequence_frame(
            doc, (uint32_t)sequence_frame, fmt);
        if (!img) {
            fprintf(stderr, "sequence frame decode failed\n");
            rc = 1;
            goto done;
        }
        if (out_path && write_ppm(out_path, img) != 0) {
            fprintf(stderr, "write %s failed\n", out_path);
            heic_image_destroy(ctx, img);
            rc = 1;
            goto done;
        }
        printf("sequence frame %d decoded %ux%u%s%s\n", sequence_frame,
               (unsigned)img->width, (unsigned)img->height,
               out_path ? " wrote " : "", out_path ? out_path : "");
        heic_image_destroy(ctx, img);
        rc = 0;
    }

    if (do_exif) {
        uint8_t *exif = NULL;
        size_t elen = 0;
        if (heic_doc_exif(doc, &exif, &elen)) {
            printf("exif %zu bytes\n", elen);
            heic_free(ctx, exif);
            rc = 0;
        } else {
            printf("exif none\n");
            rc = 0;
        }
    }

    if (do_decode) {
        heic_format fmt = want_rgba ? HEIC_FORMAT_RGBA : HEIC_FORMAT_RGB;
        heic_image *img = do_thumbnail
            ? heic_doc_decode_thumbnail(doc, fmt)
            : heic_doc_decode(doc, fmt);
        if (!img) {
            fprintf(stderr, "decode failed\n");
            rc = 1;
            goto done;
        }
        if (write_ppm(out_path, img) != 0) {
            fprintf(stderr, "write %s failed\n", out_path);
            heic_image_destroy(ctx, img);
            rc = 1;
            goto done;
        }
        printf("wrote %s %ux%u fmt=%s\n", out_path, (unsigned)img->width,
               (unsigned)img->height, want_rgba ? "rgba" : "rgb");
        heic_image_destroy(ctx, img);
        rc = 0;
    }

done:
    heic_doc_close(doc);
    heic_ctx_free(ctx);
    free(data);
    return rc;
}
