/* bench_wic.h -- Windows Imaging Component (WIC) session timing for heic_test.
 *
 * Windows-only. Used by `heic_test -bench` to compare against the OS HEIF/AVIF
 * codec (Microsoft HEIF/AVIF Image Extensions when installed).
 */
#ifndef HEIC_BENCH_WIC_H
#define HEIC_BENCH_WIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct heic_wic_session {
    double total_ms;
    uint32_t width, height;
    int ok;
    char error[256];
} heic_wic_session;

/* Open from memory via WIC, decode frame 0 to BGRA, free. Disk I/O outside.
 * Times the full session only (no open/decode/close split). */
int heic_bench_wic_session(const uint8_t *data, size_t len, heic_wic_session *out);

#ifdef __cplusplus
}
#endif

#endif
