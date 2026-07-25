/* unci.c -- ISO 23001-17 uncompressed HEIF (item type 'unci')
 *
 * Supports (aligned with libheif unc_decoder_* for common fixtures):
 *   - interleave 0 planar, 1 pixel, 2 mixed (semi-planar), 3 row, 4 tile-comp
 *   - multi-tile grids (num_tile_cols/rows)
 *   - 1..16 bit unsigned components with component_align_size
 *   - sampling 0 (444), 1 (422), 2 (420); mixed needs 422/420
 *   - pixel_size pad on pixel interleave (psz5/psz10 fixtures)
 *   - block_size≠0 packing: component (interleave 0) and pixel (interleave 1)
 *   - zlib/deflate (HEIC_HAVE_ZLIB), brotli (HEIC_HAVE_BROTLI); icef multi-unit
 *
 * GBR identity (matrix 0): Y=G, Cb=B, Cr=R for RGB types.
 * Y/Cb/Cr map to planes 0/1/2; subsampled chroma uses reduced tile size.
 */
#include "heic_internal.h"

#ifdef HEIC_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef HEIC_HAVE_BROTLI
#include <brotli/decode.h>
#endif

enum {
    HEIC_UNCI_MONO = 0,
    HEIC_UNCI_Y = 1,
    HEIC_UNCI_CB = 2,
    HEIC_UNCI_CR = 3,
    HEIC_UNCI_R = 4,
    HEIC_UNCI_G = 5,
    HEIC_UNCI_B = 6,
    HEIC_UNCI_A = 7,
    HEIC_UNCI_PAD = 8
};

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         bit_pos; /* absolute bit index */
} unci_br;

static void br_init(unci_br *br, const uint8_t *d, size_t n)
{
    br->data = d;
    br->len = n;
    br->bit_pos = 0;
}

static void br_skip_bits(unci_br *br, int n)
{
    if (n > 0) br->bit_pos += (size_t)n;
}

static void br_byte_align(unci_br *br)
{
    if (br->bit_pos & 7)
        br->bit_pos = (br->bit_pos + 7) & ~(size_t)7;
}

/* Big-endian bit read (MSB first), matches libheif BitReader::get_bits. */
static int br_get_bits(unci_br *br, int n)
{
    int v = 0, i;
    if (n <= 0 || n > 16) return 0;
    for (i = 0; i < n; i++) {
        size_t byte = br->bit_pos >> 3;
        int bit = 7 - (int)(br->bit_pos & 7);
        int b = 0;
        if (byte < br->len) b = (br->data[byte] >> bit) & 1;
        v = (v << 1) | b;
        br->bit_pos++;
    }
    return v;
}

static void br_skip_bytes(unci_br *br, size_t n)
{
    br_byte_align(br);
    br->bit_pos += n * 8;
}

static int type_to_plane(uint16_t t)
{
    switch (t) {
    case HEIC_UNCI_MONO:
    case HEIC_UNCI_Y:
    case HEIC_UNCI_G:
        return 0;
    case HEIC_UNCI_CB:
    case HEIC_UNCI_B:
        return 1;
    case HEIC_UNCI_CR:
    case HEIC_UNCI_R:
        return 2;
    case HEIC_UNCI_A:
        return 3;
    default:
        return -1; /* pad / unknown */
    }
}

static uint16_t resolve_type(const heic_uncc_comp *c, const heic_cmpd *cmpd)
{
    uint16_t idx = c->component_index;
    if (cmpd && cmpd->types && idx < (uint16_t)cmpd->n_types)
        return cmpd->types[idx];
    return idx;
}

/* Expand sample to 8-bit (libheif: high bits for depth>8; full-scale for depth<8). */
static uint16_t expand8(int val, int bit_depth)
{
    int maxv;
    if (bit_depth > 8) return (uint16_t)((val >> (bit_depth - 8)) & 0xFF);
    if (bit_depth == 8) return (uint16_t)(val & 0xFF);
    if (bit_depth <= 0) return 0;
    maxv = (1 << bit_depth) - 1;
    if (maxv <= 0) return 0;
    return (uint16_t)((val * 255 + maxv / 2) / maxv);
}

/* Growable append of decompressed bytes; *dst may be reallocated. */
static int unci_append_bytes(heic_ctx *ctx, uint8_t **dst, size_t *dst_len, size_t *dst_cap,
                             const uint8_t *src, size_t n)
{
    size_t need;
    uint8_t *p;
    if (!n) return 0;
    need = *dst_len + n;
    if (need < *dst_len) return -1;
    if (need > *dst_cap) {
        size_t ncap = *dst_cap ? *dst_cap : 4096;
        while (ncap < need) {
            if (ncap > (SIZE_MAX / 2)) return -1;
            ncap *= 2;
        }
        p = (uint8_t *)heic_realloc_buf(ctx, *dst, *dst_cap, ncap);
        if (!p) return -1;
        *dst = p;
        *dst_cap = ncap;
    }
    memcpy(*dst + *dst_len, src, n);
    *dst_len = need;
    return 0;
}

#ifdef HEIC_HAVE_ZLIB
static int inflate_zlib_or_deflate(heic_ctx *ctx, const uint8_t *src, size_t src_len,
                                   uint8_t **dst, size_t *dst_len, size_t *dst_cap,
                                   int raw_deflate)
{
    z_stream z;
    int rc;
    uint8_t chunk[8192];

    memset(&z, 0, sizeof(z));
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)src_len;
    /* zlib window 15; raw deflate -15 (matches libheif compression_zlib.cc). */
    rc = inflateInit2(&z, raw_deflate ? -15 : 15);
    if (rc != Z_OK) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: inflateInit failed");
        return -1;
    }
    for (;;) {
        z.next_out = (Bytef *)chunk;
        z.avail_out = (uInt)sizeof(chunk);
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc == Z_STREAM_ERROR || rc == Z_DATA_ERROR || rc == Z_NEED_DICT) {
            inflateEnd(&z);
            heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: zlib/deflate decompress failed");
            return -1;
        }
        {
            size_t got = sizeof(chunk) - (size_t)z.avail_out;
            if (got && unci_append_bytes(ctx, dst, dst_len, dst_cap, chunk, got) != 0) {
                inflateEnd(&z);
                return -1;
            }
        }
        if (rc == Z_STREAM_END) break;
        if (rc == Z_BUF_ERROR && z.avail_in == 0) break;
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&z);
            heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: zlib/deflate decompress failed");
            return -1;
        }
    }
    inflateEnd(&z);
    return 0;
}
#endif

#ifdef HEIC_HAVE_BROTLI
static int inflate_brotli(heic_ctx *ctx, const uint8_t *src, size_t src_len,
                          uint8_t **dst, size_t *dst_len, size_t *dst_cap)
{
    BrotliDecoderState *st;
    BrotliDecoderResult res;
    const uint8_t *next_in = src;
    size_t avail_in = src_len;
    uint8_t chunk[8192];
    uint8_t *next_out = chunk;
    size_t avail_out = sizeof(chunk);

    st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!st) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: brotli init failed");
        return -1;
    }
    for (;;) {
        res = BrotliDecoderDecompressStream(st, &avail_in, &next_in, &avail_out, &next_out,
                                            NULL);
        if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT ||
            res == BROTLI_DECODER_RESULT_SUCCESS) {
            size_t got = sizeof(chunk) - avail_out;
            if (got && unci_append_bytes(ctx, dst, dst_len, dst_cap, chunk, got) != 0) {
                BrotliDecoderDestroyInstance(st);
                return -1;
            }
            next_out = chunk;
            avail_out = sizeof(chunk);
            if (res == BROTLI_DECODER_RESULT_SUCCESS) break;
        } else if (res == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            BrotliDecoderDestroyInstance(st);
            heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: brotli truncated input");
            return -1;
        } else {
            BrotliDecoderDestroyInstance(st);
            heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: brotli decompress failed");
            return -1;
        }
    }
    BrotliDecoderDestroyInstance(st);
    return 0;
}
#endif

static int unci_decompress_unit(heic_ctx *ctx, heic_fourcc ct, const uint8_t *src,
                                size_t src_len, uint8_t **dst, size_t *dst_len,
                                size_t *dst_cap)
{
    if (ct == HEIC_FCC('z', 'l', 'i', 'b')) {
#ifdef HEIC_HAVE_ZLIB
        return inflate_zlib_or_deflate(ctx, src, src_len, dst, dst_len, dst_cap, 0);
#else
        (void)src;
        (void)src_len;
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: zlib needs HEIC_HAVE_ZLIB");
        return -1;
#endif
    }
    if (ct == HEIC_FCC('d', 'e', 'f', 'l')) {
#ifdef HEIC_HAVE_ZLIB
        return inflate_zlib_or_deflate(ctx, src, src_len, dst, dst_len, dst_cap, 1);
#else
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: deflate needs HEIC_HAVE_ZLIB");
        return -1;
#endif
    }
    if (ct == HEIC_FCC('b', 'r', 'o', 't')) {
#ifdef HEIC_HAVE_BROTLI
        return inflate_brotli(ctx, src, src_len, dst, dst_len, dst_cap);
#else
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: brotli needs HEIC_HAVE_BROTLI");
        return -1;
#endif
    }
    heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: compression type not supported");
    return -1;
}

/* Decompress cmpC payload (optional icef multi-unit) into a single buffer. */
static int unci_decompress_payload(heic_ctx *ctx, const heic_cmpc *cmpc,
                                   const heic_icef *icef, const uint8_t *data,
                                   size_t len, uint8_t **out_buf, size_t *out_len)
{
    uint8_t *dst = NULL;
    size_t dst_len = 0, dst_cap = 0;
    heic_fourcc ct;

    if (!cmpc || !data || !out_buf || !out_len) return -1;
    *out_buf = NULL;
    *out_len = 0;
    ct = cmpc->compression_type;

    if (icef && icef->n_units > 0 && icef->units) {
        int i;
        for (i = 0; i < icef->n_units; i++) {
            uint64_t off = icef->units[i].offset;
            uint64_t sz = icef->units[i].size;
            if (off > len || sz > len - off) {
                heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: icef unit out of range");
                heic_free_buf(ctx, dst);
                return -1;
            }
            if (unci_decompress_unit(ctx, ct, data + (size_t)off, (size_t)sz, &dst,
                                     &dst_len, &dst_cap) != 0) {
                heic_free_buf(ctx, dst);
                return -1;
            }
        }
    } else {
        /* Full-item / single blob (unit_type 0 or empty icef). */
        if (unci_decompress_unit(ctx, ct, data, len, &dst, &dst_len, &dst_cap) != 0) {
            heic_free_buf(ctx, dst);
            return -1;
        }
    }
    *out_buf = dst;
    *out_len = dst_len;
    return 0;
}

/* Read one component sample with optional component_align_size (libheif). */
static int read_comp_sample(unci_br *br, int bit_depth, uint8_t align)
{
    if (align) {
        int pad = (int)align * 8 - bit_depth;
        br_byte_align(br);
        if (pad > 0) br_skip_bits(br, pad);
    }
    return br_get_bits(br, bit_depth);
}

static void store_sample(heic_frame *out, int plane, uint32_t x, uint32_t y, uint16_t v8)
{
    size_t di;
    if (plane == 0) {
        di = (size_t)y * (size_t)out->y_stride + x;
        if (di < (size_t)out->y_stride * (size_t)out->height) out->y[di] = v8;
    } else if (plane == 1) {
        di = (size_t)y * (size_t)out->c_stride + x;
        if (di < (size_t)out->c_stride * (size_t)out->c_height) out->cb[di] = v8;
    } else if (plane == 2) {
        di = (size_t)y * (size_t)out->c_stride + x;
        if (di < (size_t)out->c_stride * (size_t)out->c_height) out->cr[di] = v8;
    } else if (plane == 3 && out->a) {
        di = (size_t)y * (size_t)out->a_stride + x;
        if (di < (size_t)out->a_stride * (size_t)out->height) out->a[di] = v8;
    }
}

/* Luma tile size → chroma tile size for sampling_type (0=444, 1=422, 2=420). */
static void plane_tile_dims(int plane, int sampling, uint32_t tw, uint32_t th,
                            uint32_t *pw, uint32_t *ph)
{
    if (plane == 1 || plane == 2) {
        if (sampling == 1) { /* 4:2:2 */
            *pw = tw / 2;
            *ph = th;
            return;
        }
        if (sampling == 2) { /* 4:2:0 */
            *pw = tw / 2;
            *ph = th / 2;
            return;
        }
    }
    *pw = tw;
    *ph = th;
}

static int sampling_to_chroma(int sampling)
{
    if (sampling == 1) return 2; /* 422 */
    if (sampling == 2) return 1; /* 420 */
    return 3;                    /* 444 */
}

/* Read block_size (1..8) bytes at current byte-aligned position into u64. */
static int br_read_block_u64(unci_br *br, uint8_t block_size, int little_endian,
                             uint64_t *out)
{
    size_t byte;
    uint32_t b;
    uint64_t v = 0;
    br_byte_align(br);
    byte = br->bit_pos >> 3;
    if (block_size == 0 || block_size > 8) return -1;
    if (byte + block_size > br->len) return -1;
    if (little_endian) {
        for (b = 0; b < block_size; b++)
            v |= (uint64_t)br->data[byte + b] << (b * 8);
    } else {
        for (b = 0; b < block_size; b++)
            v = (v << 8) | br->data[byte + b];
    }
    br->bit_pos += (size_t)block_size * 8u;
    *out = v;
    return 0;
}

/* Block + component interleave (libheif unc_decoder_block_component_interleave). */
static int decode_block_component(heic_ctx *ctx, const heic_uncc *uncc,
                                  const int *plane_map, const int *bit_depth,
                                  int ncomp, uint32_t tile_cols, uint32_t tile_rows,
                                  uint32_t tile_w, uint32_t tile_h, unci_br *br,
                                  heic_frame *out, const heic_abort *ab)
{
    uint8_t bsz = uncc->block_size;
    uint32_t ty, tx, y, x;
    int i;
    if (bsz < 1 || bsz > 8 || uncc->pixel_size != 0 || uncc->sampling_type != 0 ||
        uncc->components_little_endian) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "unci: block+component constraints not met");
        return -1;
    }
    for (i = 0; i < ncomp; i++) {
        int bd = bit_depth[i];
        int bits = (int)bsz * 8;
        if (bd <= bits / 2 || bd > bits) {
            heic_error(ctx, HEIC_SEVERITY_ERROR,
                       "unci: block component bit depth out of range");
            return -1;
        }
    }
    for (ty = 0; ty < tile_rows; ty++) {
        for (tx = 0; tx < tile_cols; tx++) {
            size_t tile_start;
            if (heic_abort_check(ab)) return -1;
            br_byte_align(br);
            tile_start = br->bit_pos >> 3;
            for (i = 0; i < ncomp; i++) {
                int plane = plane_map[i];
                int bd = bit_depth[i];
                uint32_t shift =
                    uncc->block_pad_lsb ? (uint32_t)(bsz * 8 - bd) : 0;
                uint64_t mask = (bd >= 64) ? ~0ull : ((1ull << bd) - 1ull);
                uint32_t ox = tx * tile_w, oy = ty * tile_h;
                for (y = 0; y < tile_h; y++) {
                    size_t row_start = br->bit_pos >> 3;
                    for (x = 0; x < tile_w; x++) {
                        uint64_t blk;
                        uint32_t val;
                        if (br_read_block_u64(br, bsz, uncc->block_little_endian,
                                              &blk) != 0)
                            return -1;
                        val = (uint32_t)((blk >> shift) & mask);
                        if (plane >= 0)
                            store_sample(out, plane, ox + x, oy + y,
                                         expand8((int)val, bd));
                    }
                    br_byte_align(br);
                    if (uncc->row_align_size) {
                        size_t row_bytes = (br->bit_pos >> 3) - row_start;
                        if (row_bytes % uncc->row_align_size)
                            br_skip_bytes(br, uncc->row_align_size -
                                                   (row_bytes % uncc->row_align_size));
                    }
                }
            }
            if (uncc->tile_align_size) {
                size_t tile_bytes = (br->bit_pos >> 3) - tile_start;
                if (tile_bytes % uncc->tile_align_size)
                    br_skip_bytes(br, uncc->tile_align_size -
                                           (tile_bytes % uncc->tile_align_size));
            }
        }
    }
    return 0;
}

/* Block + pixel interleave (libheif unc_decoder_block_pixel_interleave). */
static int decode_block_pixel(heic_ctx *ctx, const heic_uncc *uncc,
                              const int *plane_map, const int *bit_depth, int ncomp,
                              uint32_t tile_cols, uint32_t tile_rows, uint32_t tile_w,
                              uint32_t tile_h, unci_br *br, heic_frame *out,
                              const heic_abort *ab)
{
    uint8_t bsz = uncc->block_size;
    uint32_t psz = uncc->pixel_size;
    uint32_t shifts[16];
    uint64_t masks[16];
    uint32_t ty, tx, y, x;
    int i;
    if (bsz == 0) bsz = (uint8_t)(psz > 255 ? 0 : psz);
    if (bsz < 1 || bsz > 8 || psz == 0 || uncc->sampling_type != 0 ||
        uncc->components_little_endian) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: block+pixel constraints not met");
        return -1;
    }
    if (uncc->block_size != 0 && uncc->block_size != psz && psz < uncc->block_size) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: pixel_size < block_size");
        return -1;
    }
    /* Component bitfield placement (mirror libheif). */
    if (!uncc->block_pad_lsb) {
        uint32_t bit_offset = 0;
        for (i = 0; i < ncomp; i++) {
            int idx = uncc->block_reversed ? i : (ncomp - 1 - i);
            shifts[idx] = bit_offset;
            bit_offset += (uint32_t)bit_depth[idx];
            masks[idx] = (bit_depth[idx] >= 64)
                             ? ~0ull
                             : ((1ull << bit_depth[idx]) - 1ull);
        }
    } else {
        uint32_t total_bits = (uint32_t)bsz * 8u;
        uint32_t bit_offset = total_bits;
        for (i = 0; i < ncomp; i++) {
            int idx = uncc->block_reversed ? i : (ncomp - 1 - i);
            bit_offset -= (uint32_t)bit_depth[idx];
            shifts[idx] = bit_offset;
            masks[idx] = (bit_depth[idx] >= 64)
                             ? ~0ull
                             : ((1ull << bit_depth[idx]) - 1ull);
        }
    }

    for (ty = 0; ty < tile_rows; ty++) {
        for (tx = 0; tx < tile_cols; tx++) {
            size_t tile_start;
            if (heic_abort_check(ab)) return -1;
            br_byte_align(br);
            tile_start = br->bit_pos >> 3;
            for (y = 0; y < tile_h; y++) {
                size_t row_start = br->bit_pos >> 3;
                for (x = 0; x < tile_w; x++) {
                    uint64_t blk;
                    if (br_read_block_u64(br, bsz, uncc->block_little_endian, &blk) != 0)
                        return -1;
                    for (i = 0; i < ncomp; i++) {
                        int plane = plane_map[i];
                        uint32_t val =
                            (uint32_t)((blk >> shifts[i]) & masks[i]);
                        if (plane >= 0)
                            store_sample(out, plane, tx * tile_w + x, ty * tile_h + y,
                                         expand8((int)val, bit_depth[i]));
                    }
                    if (psz > bsz) br_skip_bytes(br, (size_t)(psz - bsz));
                }
                br_byte_align(br);
                if (uncc->row_align_size) {
                    size_t row_bytes = (br->bit_pos >> 3) - row_start;
                    if (row_bytes % uncc->row_align_size)
                        br_skip_bytes(br, uncc->row_align_size -
                                               (row_bytes % uncc->row_align_size));
                }
            }
            if (uncc->tile_align_size) {
                size_t tile_bytes = (br->bit_pos >> 3) - tile_start;
                if (tile_bytes % uncc->tile_align_size)
                    br_skip_bytes(br, uncc->tile_align_size -
                                           (tile_bytes % uncc->tile_align_size));
            }
        }
    }
    return 0;
}

int heic_unci_decode(heic_ctx *ctx, const heic_uncc *uncc, const heic_cmpc *cmpc,
                     const heic_cmpd *cmpd, const heic_icef *icef,
                     const uint8_t *data, size_t len, uint32_t width,
                     uint32_t height, heic_frame *out, const heic_abort *ab)
{
    int i, ncomp, has_rgb = 0, has_alpha = 0, has_yuv = 0;
    int plane_map[16], bit_depth[16];
    uint8_t align[16];
    uint32_t tile_cols, tile_rows, tile_w, tile_h;
    uint8_t *owned = NULL;
    const uint8_t *pix;
    size_t pix_len;
    unci_br br;
    uint32_t ty, tx, y, x;
    int interleave, sampling, chroma_fmt;

    memset(out, 0, sizeof(*out));
    if (!ctx || !uncc || !data || !width || !height) return -1;
    if (heic_abort_check(ab)) return -1;

    ncomp = uncc->n_components;
    if (ncomp <= 0 || ncomp > 16 || !uncc->components) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: bad component count");
        return -1;
    }
    sampling = uncc->sampling_type;
    if (sampling != 0 && sampling != 1 && sampling != 2) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: sampling type %d not supported",
                   sampling);
        return -1;
    }
    interleave = uncc->interleave_type;
    if (interleave != 0 && interleave != 1 && interleave != 2 && interleave != 3 &&
        interleave != 4) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "unci: interleave type %d not supported", interleave);
        return -1;
    }
    /* Mixed (2) is semi-planar chroma (422/420 only). */
    if (interleave == 2 && sampling != 1 && sampling != 2) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "unci: mixed interleave needs 4:2:2 or 4:2:0");
        return -1;
    }
    /* Subsampled chroma: planar, mixed, or tile-component. */
    if (sampling != 0 && interleave != 0 && interleave != 2 && interleave != 4) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "unci: subsampled chroma needs planar/mixed/tile-component");
        return -1;
    }
    /* pixel_size pads each pixel to N bytes (ISO 23001-17); only for pixel interleave. */
    if (uncc->pixel_size != 0 && interleave != 1) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "unci: pixel_size only with pixel interleave");
        return -1;
    }
    if (uncc->block_size != 0 && interleave != 0 && interleave != 1) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "unci: blocked layout only for component/pixel interleave");
        return -1;
    }

    for (i = 0; i < ncomp; i++) {
        const heic_uncc_comp *c = &uncc->components[i];
        uint16_t typ;
        int bd = (int)c->component_bit_depth_minus_one + 1;
        if (bd < 1 || bd > 16 || c->component_format != 0) {
            heic_error(ctx, HEIC_SEVERITY_ERROR,
                       "unci: only 1..16-bit unsigned integer components");
            return -1;
        }
        bit_depth[i] = bd;
        align[i] = c->component_align_size;
        typ = resolve_type(c, cmpd);
        if (typ >= 4 && typ <= 6) has_rgb = 1;
        if (typ >= 1 && typ <= 3) has_yuv = 1;
        if (typ == HEIC_UNCI_A) has_alpha = 1;
        plane_map[i] = type_to_plane(typ);
    }

    tile_cols = uncc->num_tile_cols_minus_one + 1;
    tile_rows = uncc->num_tile_rows_minus_one + 1;
    if (tile_cols == 0 || tile_rows == 0 || width % tile_cols || height % tile_rows) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: invalid tile grid");
        return -1;
    }
    tile_w = width / tile_cols;
    tile_h = height / tile_rows;
    if (!tile_w || !tile_h) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: zero tile size");
        return -1;
    }
    if (sampling == 1 && (tile_w & 1)) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: 4:2:2 needs even tile width");
        return -1;
    }
    if (sampling == 2 && ((tile_w & 1) || (tile_h & 1))) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "unci: 4:2:0 needs even tile size");
        return -1;
    }

    /* Decompress if needed; raw uses input buffer as-is. */
    pix = data;
    pix_len = len;
    if (cmpc) {
        if (unci_decompress_payload(ctx, cmpc, icef, data, len, &owned, &pix_len) != 0)
            return -1;
        pix = owned;
    }

    chroma_fmt = sampling_to_chroma(sampling);
    if (heic_frame_alloc(ctx, out, (int)width, (int)height, 8, chroma_fmt) != 0) {
        heic_free_buf(ctx, owned);
        return -1;
    }
    out->full_range = 1;
    /* RGB identity = matrix 0. YUV without nclx: libheif sRGB defaults → BT.601 (6). */
    out->matrix_coeffs = has_rgb ? 0 : (has_yuv || ncomp == 1) ? 6 : 0;
    out->color_primaries = 1;
    out->transfer_characteristics = 13;

    if (has_alpha) {
        size_t a_n = (size_t)width * height * sizeof(uint16_t);
        size_t k;
        out->a = (uint16_t *)heic_zalloc(ctx, a_n);
        if (!out->a) goto fail;
        out->a_stride = (int)width;
        for (k = 0; k < (size_t)width * height; k++) out->a[k] = 255;
    }

    br_init(&br, pix, pix_len);

    /* ---- block packing (block_size ≠ 0) ---- */
    if (uncc->block_size != 0) {
        int brc;
        if (interleave == 0)
            brc = decode_block_component(ctx, uncc, plane_map, bit_depth, ncomp,
                                         tile_cols, tile_rows, tile_w, tile_h, &br,
                                         out, ab);
        else
            brc = decode_block_pixel(ctx, uncc, plane_map, bit_depth, ncomp,
                                     tile_cols, tile_rows, tile_w, tile_h, &br, out,
                                     ab);
        if (brc != 0) goto fail;
        goto done;
    }

    /* ---- tile-component (4): each component's tiles are contiguous ---- */
    if (interleave == 4) {
        for (i = 0; i < ncomp; i++) {
            uint32_t tr, tc, ptw, pth;
            int plane = plane_map[i];
            if (heic_abort_check(ab)) goto fail;
            plane_tile_dims(plane, sampling, tile_w, tile_h, &ptw, &pth);
            for (tr = 0; tr < tile_rows; tr++) {
                for (tc = 0; tc < tile_cols; tc++) {
                    uint32_t ox = tc * ptw, oy = tr * pth;
                    size_t tile_start_bytes;
                    br_byte_align(&br);
                    tile_start_bytes = br.bit_pos >> 3;
                    for (y = 0; y < pth; y++) {
                        size_t row_start = br.bit_pos >> 3;
                        for (x = 0; x < ptw; x++) {
                            int val = read_comp_sample(&br, bit_depth[i], align[i]);
                            uint16_t v8 = expand8(val, bit_depth[i]);
                            if (plane >= 0)
                                store_sample(out, plane, ox + x, oy + y, v8);
                        }
                        br_byte_align(&br);
                        if (uncc->row_align_size) {
                            size_t row_bytes = (br.bit_pos >> 3) - row_start;
                            if (row_bytes % uncc->row_align_size)
                                br_skip_bytes(&br, uncc->row_align_size -
                                                       (row_bytes % uncc->row_align_size));
                        }
                    }
                    if (uncc->tile_align_size) {
                        size_t tile_bytes = (br.bit_pos >> 3) - tile_start_bytes;
                        if (tile_bytes % uncc->tile_align_size)
                            br_skip_bytes(&br, uncc->tile_align_size -
                                                   (tile_bytes % uncc->tile_align_size));
                    }
                }
            }
        }
        goto done;
    }

    /* ---- raster of tiles: component (0), mixed (2), pixel (1), or row (3) ---- */
    for (ty = 0; ty < tile_rows; ty++) {
        for (tx = 0; tx < tile_cols; tx++) {
            size_t tile_start_bytes;
            if (heic_abort_check(ab)) goto fail;
            br_byte_align(&br);
            tile_start_bytes = br.bit_pos >> 3;

            if (interleave == 0) {
                /* Component planar within tile: C0 plane, then C1, … */
                for (i = 0; i < ncomp; i++) {
                    uint32_t ptw, pth, ox, oy;
                    int plane = plane_map[i];
                    plane_tile_dims(plane, sampling, tile_w, tile_h, &ptw, &pth);
                    ox = tx * ptw;
                    oy = ty * pth;
                    for (y = 0; y < pth; y++) {
                        size_t row_start = br.bit_pos >> 3;
                        for (x = 0; x < ptw; x++) {
                            int val = read_comp_sample(&br, bit_depth[i], align[i]);
                            uint16_t v8 = expand8(val, bit_depth[i]);
                            if (plane >= 0)
                                store_sample(out, plane, ox + x, oy + y, v8);
                        }
                        br_byte_align(&br);
                        if (uncc->row_align_size) {
                            size_t row_bytes = (br.bit_pos >> 3) - row_start;
                            if (row_bytes % uncc->row_align_size)
                                br_skip_bytes(&br, uncc->row_align_size -
                                                       (row_bytes % uncc->row_align_size));
                        }
                    }
                }
            } else if (interleave == 2) {
                /* Mixed / semi-planar: luma (or RGB) planes as planar; first chroma
                 * component streams Cb/Cr (or Cr/Cb) sample pairs for the whole
                 * chroma grid (libheif unc_decoder_mixed_interleave). */
                int chroma_done = 0;
                for (i = 0; i < ncomp; i++) {
                    uint32_t ptw, pth, ox, oy;
                    int plane = plane_map[i];
                    plane_tile_dims(plane, sampling, tile_w, tile_h, &ptw, &pth);
                    ox = tx * ptw;
                    oy = ty * pth;
                    if (plane == 1 || plane == 2) {
                        int p0 = plane, p1 = (plane == 1) ? 2 : 1;
                        int bd = bit_depth[i];
                        if (chroma_done) continue;
                        for (y = 0; y < pth; y++) {
                            for (x = 0; x < ptw; x++) {
                                /* Both samples use first chroma entry's depth (libheif). */
                                int v0 = br_get_bits(&br, bd);
                                int v1 = br_get_bits(&br, bd);
                                store_sample(out, p0, ox + x, oy + y, expand8(v0, bd));
                                store_sample(out, p1, ox + x, oy + y, expand8(v1, bd));
                            }
                            br_byte_align(&br);
                        }
                        chroma_done = 1;
                    } else {
                        for (y = 0; y < pth; y++) {
                            size_t row_start = br.bit_pos >> 3;
                            for (x = 0; x < ptw; x++) {
                                int val = read_comp_sample(&br, bit_depth[i], align[i]);
                                uint16_t v8 = expand8(val, bit_depth[i]);
                                if (plane >= 0)
                                    store_sample(out, plane, ox + x, oy + y, v8);
                            }
                            br_byte_align(&br);
                            if (uncc->row_align_size) {
                                size_t row_bytes = (br.bit_pos >> 3) - row_start;
                                if (row_bytes % uncc->row_align_size)
                                    br_skip_bytes(&br, uncc->row_align_size -
                                                           (row_bytes % uncc->row_align_size));
                            }
                        }
                    }
                }
            } else if (interleave == 1) {
                /* Pixel interleaved within tile; optional pixel_size byte padding. */
                uint32_t ox = tx * tile_w, oy = ty * tile_h;
                for (y = 0; y < tile_h; y++) {
                    size_t row_start = br.bit_pos >> 3;
                    for (x = 0; x < tile_w; x++) {
                        size_t pix_start = br.bit_pos >> 3; /* markPixelStart */
                        for (i = 0; i < ncomp; i++) {
                            int val = read_comp_sample(&br, bit_depth[i], align[i]);
                            uint16_t v8 = expand8(val, bit_depth[i]);
                            if (plane_map[i] >= 0)
                                store_sample(out, plane_map[i], ox + x, oy + y, v8);
                        }
                        if (uncc->pixel_size) {
                            size_t used;
                            br_byte_align(&br);
                            used = (br.bit_pos >> 3) - pix_start;
                            if (uncc->pixel_size < used) {
                                heic_error(ctx, HEIC_SEVERITY_ERROR,
                                           "unci: invalid pixel_size (too small)");
                                goto fail;
                            }
                            if (uncc->pixel_size > used)
                                br_skip_bytes(&br, uncc->pixel_size - used);
                        }
                    }
                    br_byte_align(&br);
                    if (uncc->row_align_size) {
                        size_t row_bytes = (br.bit_pos >> 3) - row_start;
                        if (row_bytes % uncc->row_align_size)
                            br_skip_bytes(&br, uncc->row_align_size -
                                                   (row_bytes % uncc->row_align_size));
                    }
                }
            } else {
                /* Row interleave: for each y, each component's row (libheif:
                 * markRowStart + handleRowAlignment per component, not once
                 * for the whole multi-component scanline). */
                uint32_t ox = tx * tile_w, oy = ty * tile_h;
                for (y = 0; y < tile_h; y++) {
                    for (i = 0; i < ncomp; i++) {
                        size_t row_start = br.bit_pos >> 3;
                        for (x = 0; x < tile_w; x++) {
                            int val = read_comp_sample(&br, bit_depth[i], align[i]);
                            uint16_t v8 = expand8(val, bit_depth[i]);
                            if (plane_map[i] >= 0)
                                store_sample(out, plane_map[i], ox + x, oy + y, v8);
                        }
                        br_byte_align(&br);
                        if (uncc->row_align_size) {
                            size_t row_bytes = (br.bit_pos >> 3) - row_start;
                            if (row_bytes % uncc->row_align_size)
                                br_skip_bytes(&br, uncc->row_align_size -
                                                       (row_bytes % uncc->row_align_size));
                        }
                    }
                }
            }

            if (uncc->tile_align_size) {
                size_t tile_bytes = (br.bit_pos >> 3) - tile_start_bytes;
                if (tile_bytes % uncc->tile_align_size)
                    br_skip_bytes(&br, uncc->tile_align_size -
                                           (tile_bytes % uncc->tile_align_size));
            }
        }
    }

done:
    if (!has_rgb && !has_yuv && ncomp == 1) {
        size_t k, n = (size_t)out->c_width * (size_t)out->c_height;
        for (k = 0; k < n; k++) {
            out->cb[k] = 128;
            out->cr[k] = 128;
        }
        out->matrix_coeffs = 6; /* mono → grey; BT.601 like libheif default nclx */
    }
    heic_free_buf(ctx, owned);
    return 0;

fail:
    heic_frame_free(ctx, out);
    heic_free_buf(ctx, owned);
    return -1;
}
