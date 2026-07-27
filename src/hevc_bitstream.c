/* hevc_bitstream.c -- NAL parse + bit reader (port of imazen/heic bitstream.rs) */
#include "heic_internal.h"

int heic_nal_is_slice(heic_nal_type t)
{
    return t <= HEIC_NAL_RASL_R ||
           (t >= HEIC_NAL_BLA_W_LP && t <= HEIC_NAL_CRA);
}

static heic_nal_type nal_type_from_u8(uint8_t v)
{
    if (v <= 21 || (v >= 32 && v <= 40)) return (heic_nal_type)v;
    return HEIC_NAL_UNKNOWN;
}

/* True if payload has any emulation-prevention 0x000003 sequence. */
static int rbsp_has_epb(const uint8_t *src, size_t len)
{
    size_t i;
    if (len < 3) return 0;
    /* Walk looking for 00 00 03; advance by 1 on non-zero, by 2 on 00 xx. */
    for (i = 0; i + 2 < len; ) {
        if (src[i]) {
            i++;
            continue;
        }
        if (src[i + 1]) {
            i += 2;
            continue;
        }
        /* 00 00 … */
        if (src[i + 2] == 3) return 1;
        /* 00 00 00… keep overlapping */
        i++;
    }
    return 0;
}

/* Remove emulation prevention bytes (0x000003 → 0x0000).
   Records EBSP positions of stripped 0x03 bytes into *ep_out / *n_ep. */
static int rbsp_unescape(heic_ctx *ctx, const uint8_t *src, size_t len,
                         uint8_t **out, size_t *out_len,
                         uint32_t **ep_out, int *n_ep)
{
    uint8_t *dst;
    uint32_t *eps = NULL;
    int ne = 0, cap = 0;
    size_t i, j;
    /* Uninitialized alloc — we write every byte we keep. */
    dst = (uint8_t *)heic_alloc(ctx, len ? len : 1);
    if (!dst) return -1;
    j = 0;
    for (i = 0; i < len; i++) {
        if (i + 2 < len && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3) {
            dst[j++] = 0;
            dst[j++] = 0;
            /* EP at EBSP index i+2 */
            if (ne >= cap) {
                int ncap = cap ? cap * 2 : 8;
                uint32_t *np = (uint32_t *)heic_realloc_buf(
                    ctx, eps, (size_t)cap * sizeof(uint32_t),
                    (size_t)ncap * sizeof(uint32_t));
                if (!np) {
                    heic_free_buf(ctx, dst);
                    heic_free_buf(ctx, eps);
                    return -1;
                }
                eps = np;
                cap = ncap;
            }
            eps[ne++] = (uint32_t)(i + 2);
            i += 2; /* skip 0x03 */
            continue;
        }
        dst[j++] = src[i];
    }
    *out = dst;
    *out_len = j;
    if (ep_out) *ep_out = eps;
    else heic_free_buf(ctx, eps);
    if (n_ep) *n_ep = ne;
    return 0;
}

int heic_parse_single_nal(heic_ctx *ctx, const uint8_t *data, size_t len, heic_nal *out)
{
    uint8_t nal_hdr0, nal_hdr1;
    uint8_t nal_unit_type;
    const uint8_t *payload;
    size_t payload_len;

    memset(out, 0, sizeof(*out));
    if (!data || len < 2) return -1;
    /* Optional Annex-B start code skip */
    if (len >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        data += 4;
        len -= 4;
    } else if (len >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        data += 3;
        len -= 3;
    }
    if (len < 2) return -1;
    nal_hdr0 = data[0];
    nal_hdr1 = data[1];
    if (nal_hdr0 & 0x80) return -1; /* forbidden_zero_bit */
    nal_unit_type = (nal_hdr0 >> 1) & 0x3F;
    out->type = nal_type_from_u8(nal_unit_type);
    out->nuh_layer_id = (uint8_t)(((nal_hdr0 & 1) << 5) | (nal_hdr1 >> 3));
    out->temporal_id = (nal_hdr1 & 7);
    if (out->temporal_id == 0) return -1;
    out->temporal_id -= 1;
    payload = data + 2;
    payload_len = len - 2;
    /* Common case (HEIF still slices): no EPB → zero-copy payload. */
    if (!rbsp_has_epb(payload, payload_len)) {
        out->payload = payload;
        out->payload_len = payload_len;
        out->owned = NULL;
        out->ep_positions = NULL;
        out->n_ep_positions = 0;
        return 0;
    }
    if (rbsp_unescape(ctx, payload, payload_len, &out->owned, &out->payload_len,
                      &out->ep_positions, &out->n_ep_positions) != 0)
        return -1;
    out->payload = out->owned;
    return 0;
}

void heic_nal_free(heic_ctx *ctx, heic_nal *n)
{
    if (!n) return;
    heic_free_buf(ctx, n->owned);
    heic_free_buf(ctx, n->ep_positions);
    memset(n, 0, sizeof(*n));
}

void heic_nals_free(heic_ctx *ctx, heic_nal *nals, int n)
{
    int i;
    if (!nals) return;
    for (i = 0; i < n; i++) heic_nal_free(ctx, &nals[i]);
    heic_free_buf(ctx, nals);
}

int heic_parse_length_prefixed(heic_ctx *ctx, const uint8_t *data, size_t len,
                               int length_size, heic_nal **out, int *out_n)
{
    size_t pos = 0;
    heic_nal *nals = NULL;
    int n = 0, cap = 0;

    if (!out || !out_n || length_size < 1 || length_size > 4) return -1;
    *out = NULL;
    *out_n = 0;

    while (pos + (size_t)length_size <= len) {
        size_t nalu_len = 0;
        int k;
        heic_nal nal;
        for (k = 0; k < length_size; k++)
            nalu_len = (nalu_len << 8) | data[pos + k];
        pos += (size_t)length_size;
        if (nalu_len > len - pos || nalu_len > HEIC_MAX_NAL_UNIT_SIZE) break;
        if (heic_parse_single_nal(ctx, data + pos, nalu_len, &nal) != 0) {
            pos += nalu_len;
            continue;
        }
        pos += nalu_len;
        if (n >= cap) {
            int ncap = cap ? cap * 2 : 8;
            heic_nal *nn = (heic_nal *)heic_realloc_buf(
                ctx, nals, (size_t)cap * sizeof(heic_nal),
                (size_t)ncap * sizeof(heic_nal));
            if (!nn) {
                heic_nal_free(ctx, &nal);
                heic_nals_free(ctx, nals, n);
                return -1;
            }
            nals = nn;
            cap = ncap;
        }
        nals[n++] = nal;
    }
    *out = nals;
    *out_n = n;
    return 0;
}

void heic_bs_init(heic_bs *bs, const uint8_t *data, size_t len)
{
    memset(bs, 0, sizeof(*bs));
    bs->data = data;
    bs->len = len;
}

int heic_bs_bit(heic_bs *bs)
{
    int b;
    if (!bs || bs->error || bs->byte_pos >= bs->len) {
        if (bs) bs->error = 1;
        return 0;
    }
    b = (bs->data[bs->byte_pos] >> (7 - bs->bit_pos)) & 1;
    bs->bit_pos++;
    if (bs->bit_pos == 8) {
        bs->bit_pos = 0;
        bs->byte_pos++;
    }
    return b;
}

uint32_t heic_bs_bits(heic_bs *bs, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 1) | (uint32_t)heic_bs_bit(bs);
    return v;
}

uint32_t heic_bs_ue(heic_bs *bs)
{
    int leading = 0;
    uint32_t suffix;
    while (heic_bs_bit(bs) == 0) {
        leading++;
        if (leading > 31 || bs->error) {
            bs->error = 1;
            return 0;
        }
    }
    if (leading == 0) return 0;
    suffix = heic_bs_bits(bs, leading);
    return ((1u << leading) - 1u) + suffix;
}

int32_t heic_bs_se(heic_bs *bs)
{
    uint32_t code = heic_bs_ue(bs);
    if (code == 0) return 0;
    if (code & 1) return (int32_t)((code + 1) >> 1);
    return -(int32_t)(code >> 1);
}

int heic_bs_byte_aligned(const heic_bs *bs)
{
    return bs && bs->bit_pos == 0;
}

void heic_bs_byte_align(heic_bs *bs)
{
    if (!bs) return;
    if (bs->bit_pos != 0) {
        bs->bit_pos = 0;
        bs->byte_pos++;
    }
}

size_t heic_bs_bits_left(const heic_bs *bs)
{
    if (!bs || bs->byte_pos >= bs->len) return 0;
    return (bs->len - bs->byte_pos) * 8u - (size_t)bs->bit_pos;
}
