/* hevc_cabac.c -- CABAC decoder (port of imazen/heic src/hevc/cabac.rs) */
#include "heic_internal.h"
#include "hevc_cabac_inline.h"

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

/* Precomputed init states: table × QP × contexts. Built once on first use.
 * 5 tables (I, P, B, P↔B swap for cabac_init_flag) × 52 QPs × 174 × 2 B ≈ 88 KB. */
enum {
    HEIC_CABAC_TAB_I = 0,
    HEIC_CABAC_TAB_P = 1,
    HEIC_CABAC_TAB_B = 2,
    HEIC_CABAC_TAB_P_SWAP = 3, /* P with cabac_init_flag → B inits */
    HEIC_CABAC_TAB_B_SWAP = 4,
    HEIC_CABAC_TAB_N = 5
};

static heic_ctx_model g_cabac_init_tab[HEIC_CABAC_TAB_N][52][HEIC_NUM_CONTEXTS];
static int g_cabac_init_ready;

static void cabac_fill_table(heic_ctx_model *dst, const uint8_t *table,
                             int slice_type, int slice_qp)
{
    int i;
    for (i = 0; i < HEIC_NUM_CONTEXTS; i++)
        heic_ctx_model_init(&dst[i], table[i], slice_qp);
    heic_ctx_model_init(&dst[HEIC_CTX_SIG_COEFF_FLAG_REXT],
                        slice_type == HEIC_SLICE_I ? 141 : 140, slice_qp);
    heic_ctx_model_init(&dst[HEIC_CTX_SIG_COEFF_FLAG_REXT + 1],
                        slice_type == HEIC_SLICE_I ? 111 : 140, slice_qp);
    for (i = HEIC_CTX_EXPLICIT_RDPCM_FLAG;
         i < HEIC_CTX_EXPLICIT_RDPCM_DIR + 2; i++)
        heic_ctx_model_init(&dst[i], 139, slice_qp);
    for (i = HEIC_CTX_LOG2_RES_SCALE_ABS_PLUS1;
         i < HEIC_CTX_RES_SCALE_SIGN_FLAG + 2; i++)
        heic_ctx_model_init(&dst[i], 154, slice_qp);
    heic_ctx_model_init(&dst[HEIC_CTX_CU_CHROMA_QP_OFFSET_FLAG], 154, slice_qp);
    heic_ctx_model_init(&dst[HEIC_CTX_CU_CHROMA_QP_OFFSET_IDX], 154, slice_qp);
}

static void cabac_build_init_tables(void)
{
    int qp;
    for (qp = 0; qp <= 51; qp++) {
        cabac_fill_table(g_cabac_init_tab[HEIC_CABAC_TAB_I][qp],
                         HEIC_CABAC_INIT_I, HEIC_SLICE_I, qp);
        cabac_fill_table(g_cabac_init_tab[HEIC_CABAC_TAB_P][qp],
                         HEIC_CABAC_INIT_P, HEIC_SLICE_P, qp);
        cabac_fill_table(g_cabac_init_tab[HEIC_CABAC_TAB_B][qp],
                         HEIC_CABAC_INIT_B, HEIC_SLICE_B, qp);
        cabac_fill_table(g_cabac_init_tab[HEIC_CABAC_TAB_P_SWAP][qp],
                         HEIC_CABAC_INIT_B, HEIC_SLICE_P, qp);
        cabac_fill_table(g_cabac_init_tab[HEIC_CABAC_TAB_B_SWAP][qp],
                         HEIC_CABAC_INIT_P, HEIC_SLICE_B, qp);
    }
    g_cabac_init_ready = 1;
}

void heic_cabac_init_contexts(heic_ctx_model *ctx, int slice_type, int cabac_init_flag,
                              int slice_qp)
{
    int tab, qp;
    if (!ctx) return;
    if (!g_cabac_init_ready) cabac_build_init_tables();
    qp = slice_qp;
    if (qp < 0) qp = 0;
    if (qp > 51) qp = 51;
    if (slice_type == HEIC_SLICE_I)
        tab = HEIC_CABAC_TAB_I;
    else if (slice_type == HEIC_SLICE_P)
        tab = cabac_init_flag ? HEIC_CABAC_TAB_P_SWAP : HEIC_CABAC_TAB_P;
    else
        tab = cabac_init_flag ? HEIC_CABAC_TAB_B_SWAP : HEIC_CABAC_TAB_B;
    memcpy(ctx, g_cabac_init_tab[tab][qp],
           (size_t)HEIC_NUM_CONTEXTS * sizeof(heic_ctx_model));
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
    return heic_cabac_decode_bin_i(c, ctx);
}

int heic_cabac_decode_bypass(heic_cabac *c)
{
    return heic_cabac_decode_bypass_i(c);
}

uint32_t heic_cabac_decode_bypass_bits(heic_cabac *c, int n)
{
    return heic_cabac_decode_bypass_bits_i(c, n);
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

void heic_cabac_align_bypass(heic_cabac *c)
{
    if (c && !c->error) c->range = 256;
}
