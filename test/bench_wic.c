/* bench_wic.c -- WIC (Windows Imaging Component) decode + timing for -bench.
 *
 * Fair-ish vs our/libheif session: factory + stream decoder + frame 0 +
 * convert to 32bpp BGRA + CopyPixels (touch every row). Requires a WIC HEIF
 * or AVIF codec (store extensions on older Windows).
 */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <shlwapi.h>

#include "bench_wic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

static double wic_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void set_wic_error(heic_wic_session *timing, const char *stage, HRESULT hr)
{
    size_t i;
    if (!timing) return;
    snprintf(timing->error, sizeof timing->error, "%s: HRESULT 0x%08lX", stage,
             (unsigned long)hr);
    for (i = 0; timing->error[i]; i++) {
        if (timing->error[i] == '\r' || timing->error[i] == '\n' ||
            timing->error[i] == '\t')
            timing->error[i] = ' ';
    }
    while (i > 0 && timing->error[i - 1] == ' ')
        timing->error[--i] = '\0';
}

int heic_bench_wic_session(const uint8_t *data, size_t len, heic_wic_session *out)
{
    HRESULT hr;
    IWICImagingFactory *factory = NULL;
    IStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT w = 0, h = 0, stride = 0, bufsize = 0;
    BYTE *pixels = NULL;
    double t_session;
    int rc = -1;
    UINT y;

    if (!out || !data || len == 0) return -1;

    out->total_ms = -1.0;
    out->width = out->height = 0;
    out->ok = 0;
    out->error[0] = '\0';

    t_session = wic_now_ms();

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void **)&factory);
    if (FAILED(hr) || !factory) {
        set_wic_error(out, "CoCreateInstance(WIC)", hr);
        goto done;
    }

    /* SHCreateMemStream copies when needed; stream holds a view of the buffer
     * for the lifetime of the decoder — input buffer must stay live (it does). */
    stream = SHCreateMemStream(data, (UINT)len);
    if (!stream) {
        snprintf(out->error, sizeof out->error, "open: SHCreateMemStream failed");
        goto done;
    }

    hr = factory->lpVtbl->CreateDecoderFromStream(
        factory, stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder) {
        set_wic_error(out, "CreateDecoderFromStream", hr);
        goto done;
    }

    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr) || !frame) {
        set_wic_error(out, "GetFrame", hr);
        goto done;
    }

    hr = factory->lpVtbl->CreateFormatConverter(factory, &converter);
    if (FAILED(hr) || !converter) {
        set_wic_error(out, "CreateFormatConverter", hr);
        goto done;
    }

    hr = converter->lpVtbl->Initialize(
        converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        set_wic_error(out, "FormatConverter.Initialize", hr);
        goto done;
    }

    hr = converter->lpVtbl->GetSize(converter, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        set_wic_error(out, "GetSize", hr);
        goto done;
    }
    out->width = w;
    out->height = h;

    /* Tight 32bpp BGRA row stride (4 * w), allocate and CopyPixels. */
    if (w > (UINT)(0xFFFFFFFFu / 4u)) {
        snprintf(out->error, sizeof out->error, "decode: width overflow");
        goto done;
    }
    stride = w * 4u;
    if (h > (UINT)(0xFFFFFFFFu / stride)) {
        snprintf(out->error, sizeof out->error, "decode: buffer size overflow");
        goto done;
    }
    bufsize = stride * h;
    pixels = (BYTE *)malloc(bufsize);
    if (!pixels) {
        snprintf(out->error, sizeof out->error, "decode: OOM");
        goto done;
    }

    hr = converter->lpVtbl->CopyPixels(converter, NULL, stride, bufsize, pixels);
    if (FAILED(hr)) {
        set_wic_error(out, "CopyPixels", hr);
        goto done;
    }

    /* Touch every row so decode cost is not optimized away. */
    {
        volatile BYTE sink = 0;
        for (y = 0; y < h; y++) sink ^= pixels[(size_t)y * (size_t)stride];
        (void)sink;
    }

    rc = 0;
    out->ok = 1;

done:
    if (pixels) free(pixels);
    if (converter) converter->lpVtbl->Release(converter);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);
    if (stream) stream->lpVtbl->Release(stream);
    if (factory) factory->lpVtbl->Release(factory);
    out->total_ms = wic_now_ms() - t_session;
    return rc;
}

#else /* !_WIN32 */

#include "bench_wic.h"

int heic_bench_wic_session(const uint8_t *data, size_t len, heic_wic_session *out)
{
    (void)data;
    (void)len;
    if (out) {
        out->total_ms = -1.0;
        out->width = out->height = 0;
        out->ok = 0;
        snprintf(out->error, sizeof out->error, "WIC not available on this platform");
    }
    return -1;
}

#endif
