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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
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
        if (nal_end >= nal_start + 2 && n < cap) {
            nals[n].data = data + nal_start;
            nals[n].len = nal_end - nal_start;
            nals[n].type = (uint8_t)((data[nal_start] >> 1) & 0x3f);
            n++;
        }
        if (!have_next) break;
        start = next;
        prefix = next_prefix;
    }
    return n;
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

static int do_hevc_sequence(const uint8_t *data, size_t len,
                            const char *out_path, int max_frames)
{
    annexb_nal nals[1024];
    uint8_t *params[16];
    size_t param_lens[16];
    heic_hvcc cfg;
    heic_frame frames[512];
    const heic_frame *refs[HEIC_MAX_REF_PICS];
    heic_ctx *ctx;
    int order[512];
    int n_nals, n_params = 0, decoded = 0, i, j;
    uint64_t hash = UINT64_C(1469598103934665603);

    memset(&cfg, 0, sizeof(cfg));
    memset(frames, 0, sizeof(frames));
    n_nals = split_annexb(data, len, nals, 1024);
    if (n_nals <= 0) {
        fprintf(stderr, "hevc-sequence: no Annex B NAL units\n");
        return 1;
    }
    for (i = 0; i < n_nals && n_params < 16; i++)
        if (nals[i].type >= 32 && nals[i].type <= 34) {
            params[n_params] = (uint8_t *)nals[i].data;
            param_lens[n_params++] = nals[i].len;
        }
    if (n_params < 2) {
        fprintf(stderr, "hevc-sequence: missing parameter sets\n");
        return 1;
    }
    cfg.length_size_minus_one = 3;
    cfg.nal_units = params;
    cfg.nal_unit_lens = param_lens;
    cfg.n_nal_units = n_params;
    ctx = heic_ctx_new(NULL, NULL, on_error_quiet, NULL);
    if (!ctx) return 1;

    for (i = 0; i < n_nals; i++) {
        uint8_t *sample;
        size_t sample_len;
        heic_frame out;
        if (nals[i].type > 31) continue;
        if (decoded >= max_frames) break;
        sample_len = nals[i].len + 4;
        sample = (uint8_t *)malloc(sample_len);
        if (!sample) break;
        sample[0] = (uint8_t)(nals[i].len >> 24);
        sample[1] = (uint8_t)(nals[i].len >> 16);
        sample[2] = (uint8_t)(nals[i].len >> 8);
        sample[3] = (uint8_t)nals[i].len;
        memcpy(sample + 4, nals[i].data, nals[i].len);
        int n_refs = decoded < HEIC_MAX_REF_PICS
            ? decoded : HEIC_MAX_REF_PICS;
        if (decoded >= 512) break;
        for (j = 0; j < n_refs; j++) refs[j] = &frames[decoded - 1 - j];
        memset(&out, 0, sizeof(out));
        if (heic_hevc_decode_refs(ctx, &cfg, sample, sample_len,
                                  refs, n_refs, &out, NULL) != 0) {
            fprintf(stderr, "hevc-sequence: frame %d decode failed\n",
                    decoded);
            free(sample);
            break;
        }
        free(sample);
        frames[decoded++] = out;
    }
    j = i == n_nals || decoded == max_frames;
    for (i = 0; i < decoded; i++) order[i] = i;
    for (i = 1; i < decoded; i++) {
        int oi = order[i], k = i;
        while (k > 0 && frames[order[k - 1]].poc > frames[oi].poc) {
            order[k] = order[k - 1];
            k--;
        }
        order[k] = oi;
    }
    for (i = 0; i < decoded; i++) {
        heic_frame *f = &frames[order[i]];
        hash = hash_plane(hash, f->y, f->y_stride, f->width, f->height);
        if (f->chroma_format != 0) {
            hash = hash_plane(hash, f->cb, f->c_stride,
                              f->c_width, f->c_height);
            hash = hash_plane(hash, f->cr, f->c_stride,
                              f->c_width, f->c_height);
        }
    }
    if (out_path) {
        FILE *f = fopen(out_path, "wb");
        if (!f) j = 0;
        for (i = 0; f && i < decoded; i++) {
            heic_frame *fr = &frames[order[i]];
            int plane, y, x;
            const uint16_t *planes[3] = {fr->y, fr->cb, fr->cr};
            int strides[3] = {fr->y_stride, fr->c_stride, fr->c_stride};
            int widths[3] = {fr->width, fr->c_width, fr->c_width};
            int heights[3] = {fr->height, fr->c_height, fr->c_height};
            int n_planes = fr->chroma_format ? 3 : 1;
            for (plane = 0; plane < n_planes; plane++)
                for (y = 0; y < heights[plane]; y++)
                    for (x = 0; x < widths[plane]; x++) {
                        uint8_t v = (uint8_t)planes[plane][
                            (size_t)y * strides[plane] + x];
                        if (fwrite(&v, 1, 1, f) != 1) j = 0;
                    }
        }
        if (f) fclose(f);
    }
    for (i = 0; i < decoded; i++) heic_frame_free(ctx, &frames[i]);
    heic_ctx_free(ctx);
    if (decoded == 0 || !j) return 1;
    printf("hevc-sequence frames=%d hash=%016llx\n", decoded,
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
    int r;

    printf("(bench: session open/decode/close)\n");

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
               "libheif_total=-1\n");
        printf("BENCH_LIBHEIF_ERROR %s\n", lib_error);
        printf("skip: both heic and libheif failed (unsupported / not decodable)\n");
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

            printf("(best of %d runs; + = heic slower)\n", RUNS);
            bench_print_compare_table(rows, 4);
            printf("BENCH_RESULT ours_ok=%d libheif_ok=%d "
                   "ours_open=%.4f ours_decode=%.4f ours_close=%.4f ours_total=%.4f "
                   "libheif_open=%.4f libheif_decode=%.4f libheif_close=%.4f "
                   "libheif_total=%.4f\n",
                   ours_ok, lib_ok, o_open, o_dec, o_close, o_tot,
                   l_open, l_dec, l_close, l_tot);
            if (!lib_ok) printf("BENCH_LIBHEIF_ERROR %s\n", lib_error);

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
                return 0;
            }
            if (ours_ok && !lib_ok) {
                /* Oracle reject (e.g. Nokia C021 double dimg iref) while we
                 * decode primary: report heic timings, exit 0 so bench.ts skips
                 * compare rather than failing the run. */
                printf("note: libheif failed; heic timings only (no compare)\n");
                return 0;
            }
            if (!ours_ok && lib_ok) {
                printf("note: heic failed; libheif timings only\n");
                return 1;
            }
            return 1;
        }
    }
#endif
}

#ifdef HEIC_HAVE_LIBHEIF
/* Compare RGB pixels against libheif; print mse / maxdiff.
 * Exit: 0 both ok (+ mse line), 1 heic fail libheif ok, 2 size mismatch,
 *       3 both failed, 4 libheif fail heic ok. */
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
    printf("%dx%d mse=%.4f maxdiff=%d n_diff=%d\n", rw, rh, sse / (double)n, maxd, ndiff);
    rc = 0;

done:
    if (img) heic_image_destroy(ctx, img);
    if (doc) heic_doc_close(doc);
    if (ctx) heic_ctx_free(ctx);
    free(ref);
    return rc;
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
        for (x = 0; x < rw * 3; x++) {
            int d = (int)a[x] - (int)b[x];
            if (d < 0) d = -d;
            if (d) {
                (*ndiff)++;
                *sse += (double)d * (double)d;
                if (d > *maxd) *maxd = d;
            }
        }
    }
    *compared += (uint64_t)rw * (uint64_t)rh * 3u;
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
    int rw = 0, rh = 0, rstride = 0;
    double sse = 0.0;
    uint64_t compared = 0, ndiff = 0;
    int maxd = 0, rc = 1;
    char error[256];

    if (heic_libheif_decode_sequence_rgb(
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
    decoder = heic_sequence_decoder_new(doc, HEIC_FORMAT_RGB);
    if (!decoder) goto done;
    for (frame = 0; frame < info.frame_count; frame++) {
        const heic_sequence *seq = doc->container.sequence;
        if (compare_sequence_frame(
                ctx, decoder, seq, frame, ref, ref_frames,
                rw, rh, rstride, &sse, &compared, &ndiff, &maxd) != 0)
            goto done;
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
    }
    {
        const heic_sequence *seq = doc->container.sequence;
        heic_item item;
        uint32_t sample;
        if (heic_container_get_item(
                &doc->container, doc->container.primary_item_id, &item) == 0
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
           " dependent=%d\n",
           (unsigned)info.frame_count, rw, rh, sse / (double)compared,
           maxd, (unsigned long long)ndiff, dependent);
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
static int do_profile_heic(const uint8_t *data, size_t len, int loops)
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
        img = heic_doc_decode(doc, HEIC_FORMAT_RGB);
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

int main(int argc, char **argv)
{
    const char *path = NULL;
    const char *out_path = NULL;
    int do_info = 0, do_exif = 0, do_decode = 0, do_thumbnail = 0;
    int do_bench_mode = 0, do_verify_mode = 0;
    int do_verify_sequence_mode = 0, do_verify_gain_map_mode = 0;
    int do_hevc_sequence_mode = 0;
    int do_sequence_info = 0, sequence_frame = -1;
    int profile_heic_loops = 0, profile_libheif_loops = 0;
    int hevc_sequence_frames = 512;
    int want_rgba = 0;
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
        else if (strcmp(argv[i], "-bench") == 0) do_bench_mode = 1;
        else if (strcmp(argv[i], "-verify") == 0) do_verify_mode = 1;
        else if (strcmp(argv[i], "-verify-gain-map") == 0)
            do_verify_gain_map_mode = 1;
        else if (strcmp(argv[i], "-verify-sequence") == 0)
            do_verify_sequence_mode = 1;
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
            if (hevc_sequence_frames > 512) hevc_sequence_frames = 512;
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
    }
    if (sequence_frame >= 0) do_decode = 0;
    if (!path ||
        (!do_info && !do_exif && !do_decode && !do_bench_mode
         && !do_verify_mode && !do_hevc_sequence_mode
         && !do_verify_gain_map_mode
         && !do_verify_sequence_mode
         && !do_sequence_info && sequence_frame < 0
         && !profile_heic_loops && !profile_libheif_loops)) {
        fprintf(stderr,
                "usage: heic_test [-info] [-exif] [-thumbnail] [-rgba] [-bench] [-verify] "
                "[-verify-gain-map] [-verify-sequence] "
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

    if (do_hevc_sequence_mode) {
        rc = do_hevc_sequence(data, len, out_path, hevc_sequence_frames);
        free(data);
        return rc;
    }

    if (profile_heic_loops) {
        rc = do_profile_heic(data, len, profile_heic_loops);
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
            goto done;
        }
        if (out_path && write_ppm(out_path, img) != 0) {
            fprintf(stderr, "write %s failed\n", out_path);
            heic_image_destroy(ctx, img);
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
