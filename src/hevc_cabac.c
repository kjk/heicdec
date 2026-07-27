/* hevc_cabac.c -- CABAC decoder (port of imazen/heic src/hevc/cabac.rs) */
#include "heic_internal.h"

/* ---- tables (H.265 Table 9-43 / libde265) ---- */

static const uint8_t HEIC_LPS_TABLE[64][4] = {
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216},
    {123, 150, 178, 205}, {116, 142, 169, 195}, {111, 135, 160, 185},
    {105, 128, 152, 175}, {100, 122, 144, 166}, {95, 116, 137, 158},
    {90, 110, 130, 150},  {85, 104, 123, 142},  {81, 99, 117, 135},
    {77, 94, 111, 128},   {73, 89, 105, 122},   {69, 85, 100, 116},
    {66, 80, 95, 110},    {62, 76, 90, 104},    {59, 72, 86, 99},
    {56, 69, 81, 94},     {53, 65, 77, 89},     {51, 62, 73, 85},
    {48, 59, 69, 80},     {46, 56, 66, 76},     {43, 53, 63, 72},
    {41, 50, 59, 69},     {39, 48, 56, 65},     {37, 45, 54, 62},
    {35, 43, 51, 59},     {33, 41, 48, 56},     {32, 39, 46, 53},
    {30, 37, 43, 50},     {29, 35, 41, 48},     {27, 33, 39, 45},
    {26, 31, 37, 43},     {24, 30, 35, 41},     {23, 28, 33, 39},
    {22, 27, 32, 37},     {21, 26, 30, 35},     {20, 24, 29, 33},
    {19, 23, 27, 31},     {18, 22, 26, 30},     {17, 21, 25, 28},
    {16, 20, 23, 27},     {15, 19, 22, 25},     {14, 18, 21, 24},
    {14, 17, 20, 23},     {13, 16, 19, 22},     {12, 15, 18, 21},
    {12, 14, 17, 20},     {11, 14, 16, 19},     {11, 13, 15, 18},
    {10, 12, 15, 17},     {10, 12, 14, 16},     {9, 11, 13, 15},
    {9, 11, 12, 14},      {8, 10, 12, 14},      {8, 9, 11, 13},
    {7, 9, 11, 12},       {7, 9, 10, 12},       {7, 8, 10, 11},
    {6, 8, 9, 11},        {6, 7, 9, 10},        {6, 7, 8, 9},
    {2, 2, 2, 2},
};

static const uint8_t HEIC_STATE_TRANS_MPS[64] = {
    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 62, 63,
};

static const uint8_t HEIC_STATE_TRANS_LPS[64] = {
    0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9,  11, 11, 12,
    13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
    24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
    33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63,
};

static const uint8_t HEIC_RENORM_SHIFT[32] = {
    6, 5, 4, 4, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

#include "hevc_cabac_init.inc"

void heic_ctx_model_init(heic_ctx_model *m, uint8_t init_value, int slice_qp)
{
    int slope_idx = (int)(init_value >> 4);
    int offset_idx = (int)(init_value & 15);
    int mm = slope_idx * 5 - 45;
    int nn = (offset_idx << 3) - 16;
    int qp = slice_qp;
    int init_state;
    if (qp < 0) qp = 0;
    if (qp > 51) qp = 51;
    init_state = ((mm * qp) >> 4) + nn;
    if (init_state < 1) init_state = 1;
    if (init_state > 126) init_state = 126;
    if (init_state >= 64) {
        m->state = (uint8_t)(init_state - 64);
        m->mps = 1;
    } else {
        m->state = (uint8_t)(63 - init_state);
        m->mps = 0;
    }
}

void heic_cabac_init_contexts(heic_ctx_model *ctx, int slice_type, int cabac_init_flag,
                              int slice_qp)
{
    const uint8_t *table;
    int i;
    if (slice_type == HEIC_SLICE_I)
        table = HEIC_CABAC_INIT_I;
    else if (slice_type == HEIC_SLICE_P)
        table = cabac_init_flag ? HEIC_CABAC_INIT_B : HEIC_CABAC_INIT_P;
    else
        table = cabac_init_flag ? HEIC_CABAC_INIT_P : HEIC_CABAC_INIT_B;
    for (i = 0; i < HEIC_NUM_CONTEXTS; i++)
        heic_ctx_model_init(&ctx[i], table[i], slice_qp);
    heic_ctx_model_init(&ctx[HEIC_CTX_SIG_COEFF_FLAG_REXT],
                        slice_type == HEIC_SLICE_I ? 141 : 140, slice_qp);
    heic_ctx_model_init(&ctx[HEIC_CTX_SIG_COEFF_FLAG_REXT + 1],
                        slice_type == HEIC_SLICE_I ? 111 : 140, slice_qp);
    for (i = HEIC_CTX_EXPLICIT_RDPCM_FLAG;
         i < HEIC_CTX_EXPLICIT_RDPCM_DIR + 2; i++)
        heic_ctx_model_init(&ctx[i], 139, slice_qp);
    for (i = HEIC_CTX_LOG2_RES_SCALE_ABS_PLUS1;
         i < HEIC_CTX_RES_SCALE_SIGN_FLAG + 2; i++)
        heic_ctx_model_init(&ctx[i], 154, slice_qp);
}

static int cabac_read_bit(heic_cabac *c)
{
    c->value <<= 1;
    c->bits_needed += 1;
    if (c->bits_needed >= 0) {
        if (c->byte_pos < c->len) {
            c->bits_needed = -8;
            c->value |= c->data[c->byte_pos];
            c->byte_pos++;
        } else {
            c->bits_needed = -8;
            c->overread_bytes++;
        }
    }
    return 0;
}

static int cabac_renorm(heic_cabac *c)
{
    int iters = 0;
    while (c->range < 256) {
        c->range <<= 1;
        cabac_read_bit(c);
        iters++;
        if (iters > 16) {
            c->error = 1;
            return -1;
        }
    }
    return 0;
}

int heic_cabac_new(heic_cabac *c, const uint8_t *data, size_t len)
{
    memset(c, 0, sizeof(*c));
    if (!data || len < 2) return -1;
    c->data = data;
    c->len = len;
    c->range = 510;
    c->bits_needed = -8;
    if (c->byte_pos < c->len) {
        c->value = c->data[c->byte_pos];
        c->byte_pos++;
    }
    c->value <<= 8;
    c->bits_needed = 0;
    if (c->byte_pos < c->len) {
        c->value |= c->data[c->byte_pos];
        c->byte_pos++;
        c->bits_needed = -8;
    }
    return 0;
}

void heic_cabac_seek(heic_cabac *c, size_t byte_pos)
{
    if (!c) return;
    c->byte_pos = byte_pos < c->len ? byte_pos : c->len;
    c->overread_bytes = 0;
}

void heic_cabac_reinit(heic_cabac *c)
{
    size_t remaining;
    c->range = 510;
    c->bits_needed = 8;
    c->value = 0;
    c->overread_bytes = 0;
    remaining = c->len > c->byte_pos ? c->len - c->byte_pos : 0;
    if (remaining > 0) {
        c->value = ((uint32_t)c->data[c->byte_pos]) << 8;
        c->byte_pos++;
        c->bits_needed -= 8;
    }
    if (remaining > 1) {
        c->value |= c->data[c->byte_pos];
        c->byte_pos++;
        c->bits_needed -= 8;
    }
}

int heic_cabac_overread(const heic_cabac *c)
{
    size_t slack = c->len + 256;
    return (size_t)c->overread_bytes > slack;
}

int heic_cabac_decode_bin(heic_cabac *c, heic_ctx_model *ctx)
{
    uint32_t q_range_idx, lps_range, scaled_range;
    int bin_val;
    if (c->error) return 0;
    q_range_idx = (c->range >> 6) & 3;
    lps_range = HEIC_LPS_TABLE[ctx->state][q_range_idx];
    c->range -= lps_range;
    scaled_range = c->range << 7;
    if (c->value < scaled_range) {
        bin_val = ctx->mps;
        ctx->state = HEIC_STATE_TRANS_MPS[ctx->state];
        /* The MPS range needs at most one renormalization shift. */
        if (c->range < 256) {
            c->range <<= 1;
            c->value <<= 1;
            c->bits_needed++;
            if (c->bits_needed >= 0) {
                c->bits_needed -= 8;
                if (c->byte_pos < c->len)
                    c->value |= c->data[c->byte_pos++];
                else
                    c->overread_bytes++;
            }
        }
    } else {
        uint8_t shift;
        bin_val = 1 - ctx->mps;
        c->value -= scaled_range;
        if (ctx->state == 0) ctx->mps = (uint8_t)(1 - ctx->mps);
        ctx->state = HEIC_STATE_TRANS_LPS[ctx->state];
        shift = HEIC_RENORM_SHIFT[lps_range >> 3];
        /* RangeLPS=2 or 3 is outside the table's usual valid-stream states. */
        while ((lps_range << shift) < 256) shift++;
        c->range = lps_range << shift;
        c->value <<= shift;
        c->bits_needed += shift;
        if (c->bits_needed >= 0) {
            if (c->byte_pos < c->len)
                c->value |= (uint32_t)c->data[c->byte_pos++] << c->bits_needed;
            else
                c->overread_bytes++;
            c->bits_needed -= 8;
        }
    }
    return bin_val;
}

int heic_cabac_decode_bypass(heic_cabac *c)
{
    uint32_t scaled_range;
    int bin_val;
    if (c->error) return 0;
    c->value <<= 1;
    c->bits_needed += 1;
    if (c->bits_needed >= 0) {
        if (c->byte_pos < c->len) {
            c->bits_needed = -8;
            c->value |= c->data[c->byte_pos];
            c->byte_pos++;
        } else {
            c->bits_needed = -8;
            c->overread_bytes++;
        }
    }
    scaled_range = c->range << 7;
    if (c->value >= scaled_range) {
        c->value -= scaled_range;
        bin_val = 1;
    } else
        bin_val = 0;
    return bin_val;
}

uint32_t heic_cabac_decode_bypass_bits(heic_cabac *c, int n)
{
    uint32_t result = 0;
    if (c->error || n <= 0) return 0;
    while (n > 0) {
        uint32_t scaled_range, bits, max_bits;
        int chunk = n > 8 ? 8 : n;
        c->value <<= chunk;
        c->bits_needed += chunk;
        if (c->bits_needed >= 0) {
            if (c->byte_pos < c->len) {
                c->value |= (uint32_t)c->data[c->byte_pos++] << c->bits_needed;
            } else {
                c->overread_bytes++;
            }
            c->bits_needed -= 8;
        }
        scaled_range = c->range << 7;
        bits = c->value / scaled_range;
        max_bits = (1u << chunk) - 1u;
        if (bits > max_bits) bits = max_bits;
        c->value -= bits * scaled_range;
        result = (result << chunk) | bits;
        n -= chunk;
    }
    return result;
}

int heic_cabac_decode_terminate(heic_cabac *c)
{
    uint32_t scaled_range;
    if (c->error) return 1;
    c->range -= 2;
    scaled_range = c->range << 7;
    if (c->value >= scaled_range) return 1;
    cabac_renorm(c);
    return 0;
}

uint32_t heic_cabac_decode_egk(heic_cabac *c, int k)
{
    uint32_t base = 0;
    int n = k;
    for (;;) {
        int bit = heic_cabac_decode_bypass(c);
        if (bit == 0) break;
        if (n >= 31) {
            c->error = 1;
            return 0;
        }
        base += 1u << n;
        n++;
        if (n >= k + 32) {
            c->error = 1;
            return 0;
        }
    }
    return base + heic_cabac_decode_bypass_bits(c, n);
}
