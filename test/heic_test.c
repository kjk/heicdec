/* heic_test.c -- CLI harness (jbig2dec / djvudec style)
 *
 *   heic_test -info file.heic
 *   heic_test -out out.ppm file.heic
 *   heic_test -exif file.heic
 *   heic_test -bench file.heic   # needs HEIC_HAVE_LIBHEIF (libheif + libde265)
 *   heic_test -verify file.heic  # pixel MSE vs libheif (optional)
 *   heic_test -profile-heic N file.heic     # loop decode N times (samply marks)
 *   heic_test -profile-libheif N file.heic  # same for libheif (needs oracle)
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
/* Sibling checkout: exp/samply/client — no-op when not under `samply record`. */
#include "../../samply/client/samply_control.h"
#else
#include <time.h>
static void samply_profile_start(void) {}
static void samply_profile_stop(void) {}
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
    const int RUNS = 2;
    bench_session ours[2];
#ifdef HEIC_HAVE_LIBHEIF
    heic_bench_session libraw[2];
    bench_session lib[2];
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
    if (!(ours[0].ok || ours[1].ok) && !(lib[0].ok || lib[1].ok)) {
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
        double bo = bench_best2(ours[0].open_ms, ours[1].open_ms);
        double bd = bench_best2(ours[0].decode_ms, ours[1].decode_ms);
        double bc = bench_best2(ours[0].close_ms, ours[1].close_ms);
        double bt = bench_best2(ours[0].total_ms, ours[1].total_ms);
        printf("BENCH heic open=%.4f decode=%.4f close=%.4f total=%.4f size=%ux%u ok=%d\n",
               bo, bd, bc, bt, (unsigned)(ours[0].width ? ours[0].width : ours[1].width),
               (unsigned)(ours[0].height ? ours[0].height : ours[1].height),
               ours[0].ok || ours[1].ok);
    }
    return (ours[0].ok || ours[1].ok) ? 0 : 1;
#else
    {
        bench_cmp_row rows[4];
        double o_open = bench_best2(ours[0].open_ms, ours[1].open_ms);
        double o_dec = bench_best2(ours[0].decode_ms, ours[1].decode_ms);
        double o_close = bench_best2(ours[0].close_ms, ours[1].close_ms);
        double o_tot = bench_best2(ours[0].total_ms, ours[1].total_ms);
        double l_open = bench_best2(lib[0].open_ms, lib[1].open_ms);
        double l_dec = bench_best2(lib[0].decode_ms, lib[1].decode_ms);
        double l_close = bench_best2(lib[0].close_ms, lib[1].close_ms);
        double l_tot = bench_best2(lib[0].total_ms, lib[1].total_ms);

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
            int ours_ok = ours[0].ok || ours[1].ok;
            int lib_ok = lib[0].ok || lib[1].ok;

            printf("(best of %d runs; + = heic slower)\n", RUNS);
            bench_print_compare_table(rows, 4);

            if (ours_ok && lib_ok) {
                uint32_t ow = ours[0].width ? ours[0].width : ours[1].width;
                uint32_t oh = ours[0].height ? ours[0].height : ours[1].height;
                uint32_t lw = lib[0].width ? lib[0].width : lib[1].width;
                uint32_t lh = lib[0].height ? lib[0].height : lib[1].height;
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
#endif

/* Loop open/decode/close N times with samply section marks around decode.
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
        samply_profile_start();
        t0 = bench_now_ms();
        img = heic_doc_decode(doc, HEIC_FORMAT_RGB);
        total += bench_now_ms() - t0;
        samply_profile_stop();
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
        samply_profile_start();
        if (heic_bench_libheif_session(data, len, &s) != 0 || !s.ok) {
            samply_profile_stop();
            return 1;
        }
        samply_profile_stop();
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
    int do_info = 0, do_exif = 0, do_decode = 0, do_bench_mode = 0, do_verify_mode = 0;
    int profile_heic_loops = 0, profile_libheif_loops = 0;
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
        else if (strcmp(argv[i], "-rgba") == 0) want_rgba = 1;
        else if (strcmp(argv[i], "-bench") == 0) do_bench_mode = 1;
        else if (strcmp(argv[i], "-verify") == 0) do_verify_mode = 1;
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
    if (!path ||
        (!do_info && !do_exif && !do_decode && !do_bench_mode && !do_verify_mode &&
         !profile_heic_loops && !profile_libheif_loops)) {
        fprintf(stderr,
                "usage: heic_test [-info] [-exif] [-rgba] [-bench] [-verify] "
                "[-profile-heic N] [-profile-libheif N] [-out out.ppm] file.heic\n");
        return 2;
    }

    data = read_file(path, &len);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", path);
        return 1;
    }

    heic_init();

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
    if (do_verify_mode) {
        rc = do_verify(data, len);
        free(data);
        return rc;
    }
#else
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
        printf("%ux%u kind=%s bit_depth=%d alpha=%d exif=%d xmp=%d thumb=%d\n",
               (unsigned)info.width, (unsigned)info.height, kind_str(heic_doc_kind(doc)),
               info.bit_depth, info.has_alpha, info.has_exif, info.has_xmp,
               info.has_thumbnail);
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
        heic_image *img = heic_doc_decode(doc, fmt);
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
