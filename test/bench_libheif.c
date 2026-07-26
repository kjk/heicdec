/* bench_libheif.c -- libheif (strukturag) oracle decode + timing.
 *
 * Mirrors djvudec's bench_ddjvu.cpp role: same session shape as our decoder
 * (open / decode / close) for `heic_test -bench`.
 */
#include "bench_libheif.h"

#include <libheif/heif.h>
#include <libheif/heif_sequences.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

double heic_bench_now_ms(void)
{
#ifdef _WIN32
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

static void set_bench_error(heic_bench_session *timing, const char *stage,
                            heif_error err)
{
    size_t i;
    if (!timing) return;
    snprintf(timing->error, sizeof timing->error, "%s: %s (code %d, subcode %d)",
             stage, err.message ? err.message : "unknown error",
             (int)err.code, (int)err.subcode);
    for (i = 0; timing->error[i]; i++) {
        if (timing->error[i] == '\r' || timing->error[i] == '\n' ||
            timing->error[i] == '\t')
            timing->error[i] = ' ';
    }
    while (i > 0 && timing->error[i - 1] == ' ')
        timing->error[--i] = '\0';
}

static int decode_rgb_inner(const uint8_t *data, size_t len,
                            uint8_t **out_rgb, int *out_w, int *out_h,
                            int *out_stride, heic_bench_session *timing)
{
    heif_context *ctx = NULL;
    heif_image_handle *handle = NULL;
    heif_image *image = NULL;
    heif_error err;
    const uint8_t *plane = NULL;
    int stride = 0, w = 0, h = 0, y;
    uint8_t *rgb = NULL;
    double t0 = 0.0, t_session = 0.0, t_close0 = 0.0;
    int rc = -1;

    if (timing) {
        timing->open_ms = timing->decode_ms = timing->close_ms = timing->total_ms = -1.0;
        timing->width = timing->height = 0;
        timing->ok = 0;
        timing->error[0] = '\0';
        t_session = heic_bench_now_ms();
    }

    ctx = heif_context_alloc();
    if (!ctx) {
        if (timing)
            snprintf(timing->error, sizeof timing->error,
                     "open: heif_context_alloc failed");
        goto done;
    }

    /* Fair vs our single-thread HEVC/AV1 path: one tile worker + one codec thread.
     * max_decoding_threads=0 previously left dav1d on its auto-thread default. */
    heif_context_set_max_decoding_threads(ctx, 1);

    if (timing) t0 = heic_bench_now_ms();
    err = heif_context_read_from_memory_without_copy(ctx, data, len, NULL);
    if (timing) timing->open_ms = heic_bench_now_ms() - t0;
    if (err.code != heif_error_Ok) {
        set_bench_error(timing, "open", err);
        /* Quiet: unsupported types (unci, etc.) are reported as bench skip. */
        goto done;
    }

    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) {
        if (err.code != heif_error_Ok)
            set_bench_error(timing, "primary image", err);
        else if (timing)
            snprintf(timing->error, sizeof timing->error,
                     "primary image: no primary image handle");
        goto done;
    }
    w = heif_image_handle_get_width(handle);
    h = heif_image_handle_get_height(handle);
    if (timing) {
        timing->width = (uint32_t)w;
        timing->height = (uint32_t)h;
    }

    if (timing) t0 = heic_bench_now_ms();
    {
        heif_decoding_options *opts = heif_decoding_options_alloc();
        if (opts) {
            opts->num_codec_threads = 1;
            err = heif_decode_image(handle, &image, heif_colorspace_RGB,
                                    heif_chroma_interleaved_RGB, opts);
            heif_decoding_options_free(opts);
        } else {
            err = heif_decode_image(handle, &image, heif_colorspace_RGB,
                                    heif_chroma_interleaved_RGB, NULL);
        }
    }
    if (timing) timing->decode_ms = heic_bench_now_ms() - t0;
    if (err.code != heif_error_Ok || !image) {
        set_bench_error(timing, "decode", err);
        goto done;
    }

    plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &stride);
    if (!plane || w <= 0 || h <= 0 || stride < w * 3) {
        if (timing)
            snprintf(timing->error, sizeof timing->error,
                     "decode: invalid RGB output plane");
        goto done;
    }

    if (out_rgb) {
        rgb = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
        if (!rgb) goto done;
        for (y = 0; y < h; y++) {
            memcpy(rgb + (size_t)y * (size_t)w * 3u, plane + (size_t)y * (size_t)stride,
                   (size_t)w * 3u);
        }
        *out_rgb = rgb;
        rgb = NULL;
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
        if (out_stride) *out_stride = w * 3;
    } else {
        /* Touch every row so decode cost is real (avoid dead-store elimination). */
        volatile uint8_t sink = 0;
        for (y = 0; y < h; y++) sink ^= plane[(size_t)y * (size_t)stride];
        (void)sink;
    }

    rc = 0;
    if (timing) timing->ok = 1;

done:
    if (timing) t_close0 = heic_bench_now_ms();
    if (image) heif_image_release(image);
    if (handle) heif_image_handle_release(handle);
    if (ctx) heif_context_free(ctx);
    free(rgb);
    if (timing) {
        timing->close_ms = heic_bench_now_ms() - t_close0;
        timing->total_ms = heic_bench_now_ms() - t_session;
    }
    return rc;
}

int heic_bench_libheif_session(const uint8_t *data, size_t len, heic_bench_session *out)
{
    if (!out || !data || len == 0) return -1;
    return decode_rgb_inner(data, len, NULL, NULL, NULL, NULL, out);
}

int heic_libheif_decode_rgb(const uint8_t *data, size_t len,
                            uint8_t **out_rgb, int *out_w, int *out_h, int *out_stride)
{
    if (!out_rgb) return -1;
    *out_rgb = NULL;
    return decode_rgb_inner(data, len, out_rgb, out_w, out_h, out_stride, NULL);
}

int heic_libheif_decode_sequence_rgb(const uint8_t *data, size_t len,
                                     uint8_t **out_rgb, uint32_t *out_frames,
                                     int *out_w, int *out_h, int *out_stride,
                                     char *error, size_t error_cap)
{
    heif_context *ctx = NULL;
    heif_track *track = NULL;
    heif_decoding_options *opts = NULL;
    uint8_t *rgb = NULL;
    size_t frame_size = 0, capacity = 0;
    uint32_t frames = 0;
    int width = 0, height = 0, rc = -1;
    heif_error err;

    if (!data || !len || !out_rgb || !out_frames) return -1;
    *out_rgb = NULL;
    *out_frames = 0;
    if (error && error_cap) error[0] = '\0';
    ctx = heif_context_alloc();
    if (!ctx) goto done;
    heif_context_set_max_decoding_threads(ctx, 1);
    err = heif_context_read_from_memory_without_copy(ctx, data, len, NULL);
    if (err.code != heif_error_Ok) goto heif_fail;
    track = heif_context_get_track(ctx, 0);
    if (!track) {
        if (error && error_cap) snprintf(error, error_cap, "no visual track");
        goto done;
    }
    opts = heif_decoding_options_alloc();
    if (opts) {
        opts->num_codec_threads = 1;
        opts->ignore_sequence_editlist = 1;
    }
    for (;;) {
        heif_image *image = NULL;
        const uint8_t *plane;
        int stride, w, h, y;
        err = heif_track_decode_next_image(
            track, &image, heif_colorspace_RGB,
            heif_chroma_interleaved_RGB, opts);
        if (err.code == heif_error_End_of_sequence) break;
        if (err.code != heif_error_Ok || !image) {
            if (image) heif_image_release(image);
            goto heif_fail;
        }
        plane = heif_image_get_plane_readonly(
            image, heif_channel_interleaved, &stride);
        w = heif_image_get_width(image, heif_channel_interleaved);
        h = heif_image_get_height(image, heif_channel_interleaved);
        if (!plane || w <= 0 || h <= 0 || stride < w * 3
            || (frames && (w != width || h != height))) {
            heif_image_release(image);
            if (error && error_cap)
                snprintf(error, error_cap, "invalid sequence RGB plane");
            goto done;
        }
        if (!frames) {
            width = w;
            height = h;
            if ((size_t)w > SIZE_MAX / 3
                || (size_t)w * 3 > SIZE_MAX / (size_t)h) {
                heif_image_release(image);
                goto done;
            }
            frame_size = (size_t)w * (size_t)h * 3;
        }
        if (frames == UINT32_MAX
            || (size_t)(frames + 1) > SIZE_MAX / frame_size) {
            heif_image_release(image);
            goto done;
        }
        if ((size_t)(frames + 1) * frame_size > capacity) {
            size_t needed = (size_t)(frames + 1) * frame_size;
            size_t next =
                capacity && capacity <= SIZE_MAX / 2
                    ? capacity * 2 : needed;
            uint8_t *p;
            if (next < needed) next = needed;
            p = (uint8_t *)realloc(rgb, next);
            if (!p) {
                heif_image_release(image);
                goto done;
            }
            rgb = p;
            capacity = next;
        }
        for (y = 0; y < h; y++)
            memcpy(rgb + (size_t)frames * frame_size
                       + (size_t)y * (size_t)w * 3,
                   plane + (size_t)y * (size_t)stride, (size_t)w * 3);
        frames++;
        heif_image_release(image);
    }
    if (!frames) goto done;
    *out_rgb = rgb;
    *out_frames = frames;
    if (out_w) *out_w = width;
    if (out_h) *out_h = height;
    if (out_stride) *out_stride = width * 3;
    rgb = NULL;
    rc = 0;
    goto done;

heif_fail:
    if (error && error_cap)
        snprintf(error, error_cap, "%s (code %d, subcode %d)",
                 err.message ? err.message : "libheif sequence error",
                 (int)err.code, (int)err.subcode);
done:
    free(rgb);
    if (opts) heif_decoding_options_free(opts);
    if (track) heif_track_release(track);
    if (ctx) heif_context_free(ctx);
    return rc;
}
