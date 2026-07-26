/* bench_libheif.h -- libheif oracle timing/decode for heic_test (djvudec-style).
 *
 * Built only when HEIC_HAVE_LIBHEIF is defined and linked with heif + libde265
 * (+ dav1d when AVIF is enabled in the oracle build).
 */
#ifndef HEIC_BENCH_LIBHEIF_H
#define HEIC_BENCH_LIBHEIF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct heic_bench_session {
    double open_ms;
    double decode_ms;
    double close_ms;
    double total_ms;
    uint32_t width, height;
    int ok;
    char error[256];
} heic_bench_session;

/* Monotonic ms (same clock used for our decoder timings). */
double heic_bench_now_ms(void);

/* Open from memory, decode primary to interleaved RGB8, free. Disk I/O outside. */
int heic_bench_libheif_session(const uint8_t *data, size_t len, heic_bench_session *out);

/* Decode primary image to tightly packed RGB8. Caller free()s *out_rgb. */
int heic_libheif_decode_rgb(const uint8_t *data, size_t len,
                            uint8_t **out_rgb, int *out_w, int *out_h, int *out_stride);

/* Decode one unedited media pass of the first visual sequence track to
   concatenated tightly packed RGB8 frames. Caller free()s *out_rgb. */
int heic_libheif_decode_sequence_rgb(const uint8_t *data, size_t len,
                                     uint8_t **out_rgb, uint32_t *out_frames,
                                     int *out_w, int *out_h, int *out_stride,
                                     char *error, size_t error_cap);

#ifdef __cplusplus
}
#endif

#endif
