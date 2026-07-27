/* hevc_residual.c -- residual_coding CABAC path (port of imazen/heic residual.rs) */
#include "heic_internal.h"
#include "hevc_cabac_inline.h"

/* Hot residual path: force-inlined CABAC (avoids call overhead per bin). */
#define heic_cabac_decode_bin         heic_cabac_decode_bin_i
#define heic_cabac_decode_bypass      heic_cabac_decode_bypass_i
#define heic_cabac_decode_bypass_bits heic_cabac_decode_bypass_bits_i

/* 4x4 scan tables (H.265 Table 6-7 etc.) */
static const uint8_t HEIC_SCAN_4X4_DIAG[16][2] = {
    {0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {2, 0}, {0, 3}, {1, 2},
    {2, 1}, {3, 0}, {1, 3}, {2, 2}, {3, 1}, {2, 3}, {3, 2}, {3, 3},
};
static const uint8_t HEIC_SCAN_4X4_HORIZ[16][2] = {
    {0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}, {1, 1}, {2, 1}, {3, 1},
    {0, 2}, {1, 2}, {2, 2}, {3, 2}, {0, 3}, {1, 3}, {2, 3}, {3, 3},
};
static const uint8_t HEIC_SCAN_4X4_VERT[16][2] = {
    {0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 0}, {1, 1}, {1, 2}, {1, 3},
    {2, 0}, {2, 1}, {2, 2}, {2, 3}, {3, 0}, {3, 1}, {3, 2}, {3, 3},
};

static const uint8_t HEIC_SCAN_1X1[1][2] = {{0, 0}};
static const uint8_t HEIC_SCAN_2X2_DIAG[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
static const uint8_t HEIC_SCAN_2X2_HORIZ[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
static const uint8_t HEIC_SCAN_4X4_SB_DIAG[16][2] = {
    {0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {2, 0}, {0, 3}, {1, 2},
    {2, 1}, {3, 0}, {1, 3}, {2, 2}, {3, 1}, {2, 3}, {3, 2}, {3, 3},
};
static const uint8_t HEIC_SCAN_8X8_SB_DIAG[64][2] = {
    {0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {2, 0}, {0, 3}, {1, 2},
    {2, 1}, {3, 0}, {0, 4}, {1, 3}, {2, 2}, {3, 1}, {4, 0}, {0, 5},
    {1, 4}, {2, 3}, {3, 2}, {4, 1}, {5, 0}, {0, 6}, {1, 5}, {2, 4},
    {3, 3}, {4, 2}, {5, 1}, {6, 0}, {0, 7}, {1, 6}, {2, 5}, {3, 4},
    {4, 3}, {5, 2}, {6, 1}, {7, 0}, {1, 7}, {2, 6}, {3, 5}, {4, 4},
    {5, 3}, {6, 2}, {7, 1}, {2, 7}, {3, 6}, {4, 5}, {5, 4}, {6, 3},
    {7, 2}, {3, 7}, {4, 6}, {5, 5}, {6, 4}, {7, 3}, {4, 7}, {5, 6},
    {6, 5}, {7, 4}, {5, 7}, {6, 6}, {7, 5}, {6, 7}, {7, 6}, {7, 7},
};
/* Inverse of HEIC_SCAN_8X8_SB_DIAG: index = sy*8+sx → scan position. */
static const uint8_t HEIC_INV_8X8_SB_DIAG[64] = {
     0,  2,  5,  9, 14, 20, 27, 35,  1,  4,  8, 13, 19, 26, 34, 42,
     3,  7, 12, 18, 25, 33, 41, 48,  6, 11, 17, 24, 32, 40, 47, 53,
    10, 16, 23, 31, 39, 46, 52, 57, 15, 22, 30, 38, 45, 51, 56, 60,
    21, 29, 37, 44, 50, 55, 59, 62, 28, 36, 43, 49, 54, 58, 61, 63
};

static const uint8_t HEIC_SIG_CTX_4X4[3][16] = {
    {0, 2, 1, 6, 3, 4, 7, 6, 4, 5, 7, 8, 5, 8, 8, 8},
    {0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8},
    {0, 2, 6, 7, 1, 3, 6, 7, 4, 4, 8, 8, 5, 5, 8, 8},
};

static const uint8_t HEIC_SIG_CTX_LOCAL[3][4][16] = {
    {
        {2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {2, 1, 2, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 0, 0, 0},
        {2, 2, 1, 2, 1, 0, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    },
    {
        {2, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
        {2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
        {2, 1, 0, 0, 2, 1, 0, 0, 2, 1, 0, 0, 2, 1, 0, 0},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    },
    {
        {2, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
        {2, 1, 0, 0, 2, 1, 0, 0, 2, 1, 0, 0, 2, 1, 0, 0},
        {2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0},
        {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2},
    },
};

int heic_get_scan_order(uint8_t log2_size, uint8_t intra_mode, uint8_t c_idx,
                        int chroma_444)
{
    int use_directional;
    if (c_idx == 0)
        use_directional = (log2_size == 2 || log2_size == 3);
    else
        use_directional = (log2_size == 2 || (log2_size == 3 && chroma_444));
    if (!use_directional) return HEIC_SCAN_DIAG;
    if (intra_mode >= 6 && intra_mode <= 14) return HEIC_SCAN_VERT;
    if (intra_mode >= 22 && intra_mode <= 30) return HEIC_SCAN_HORIZ;
    return HEIC_SCAN_DIAG;
}

static const uint8_t (*scan_4x4(int order))[2]
{
    if (order == HEIC_SCAN_HORIZ) return HEIC_SCAN_4X4_HORIZ;
    if (order == HEIC_SCAN_VERT) return HEIC_SCAN_4X4_VERT;
    return HEIC_SCAN_4X4_DIAG;
}

static void get_scan_sub(uint8_t log2_size, int order,
                         const uint8_t (**out)[2], int *out_n)
{
    switch (log2_size) {
    case 2:
        *out = HEIC_SCAN_1X1;
        *out_n = 1;
        break;
    case 3:
        if (order == HEIC_SCAN_HORIZ) {
            *out = HEIC_SCAN_2X2_HORIZ;
            *out_n = 4;
        } else {
            *out = HEIC_SCAN_2X2_DIAG;
            *out_n = 4;
        }
        break;
    case 4:
        *out = HEIC_SCAN_4X4_SB_DIAG;
        *out_n = 16;
        break;
    case 5:
        *out = HEIC_SCAN_8X8_SB_DIAG;
        *out_n = 64;
        break;
    default:
        *out = HEIC_SCAN_1X1;
        *out_n = 1;
        break;
    }
}

/* Inverse of HEIC_SCAN_4X4_* : index = y*4+x → scan position. */
static const uint8_t HEIC_INV_4X4_DIAG[16] = {
    0, 2, 5, 9, 1, 4, 8, 12, 3, 7, 11, 14, 6, 10, 13, 15
};
static const uint8_t HEIC_INV_4X4_HORIZ[16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
static const uint8_t HEIC_INV_4X4_VERT[16] = {
    0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15
};
/* Inverse 2x2 subblock scans (index = sy*2+sx). */
static const uint8_t HEIC_INV_2X2_DIAG[4] = {0, 2, 1, 3};
static const uint8_t HEIC_INV_2X2_HORIZ[4] = {0, 1, 2, 3};

static uint32_t find_scan_pos(const uint8_t (*scan)[2], int n, uint32_t x, uint32_t y)
{
    int i;
    /* Fast paths for fixed tables used by residual coding. */
    if (n == 1) return 0;
    if (n == 4) {
        if (scan == HEIC_SCAN_2X2_DIAG) return HEIC_INV_2X2_DIAG[(y << 1) | x];
        if (scan == HEIC_SCAN_2X2_HORIZ) return HEIC_INV_2X2_HORIZ[(y << 1) | x];
    }
    if (n == 16 && scan == HEIC_SCAN_4X4_SB_DIAG)
        return (uint32_t)HEIC_INV_4X4_DIAG[(y << 2) | x];
    if (n == 64 && scan == HEIC_SCAN_8X8_SB_DIAG)
        return (uint32_t)HEIC_INV_8X8_SB_DIAG[(y << 3) | x];
    for (i = 0; i < n; i++)
        if (scan[i][0] == x && scan[i][1] == y) return (uint32_t)i;
    return 0;
}

static uint8_t find_scan_pos_4x4(const uint8_t (*scan)[2], uint8_t x, uint8_t y)
{
    if (scan == HEIC_SCAN_4X4_DIAG) return HEIC_INV_4X4_DIAG[(y << 2) | x];
    if (scan == HEIC_SCAN_4X4_HORIZ) return HEIC_INV_4X4_HORIZ[(y << 2) | x];
    if (scan == HEIC_SCAN_4X4_VERT) return HEIC_INV_4X4_VERT[(y << 2) | x];
    {
        int i;
        for (i = 0; i < 16; i++)
            if (scan[i][0] == x && scan[i][1] == y) return (uint8_t)i;
    }
    return 0;
}

static uint32_t decode_last_sig_prefix(heic_cabac *cabac, heic_ctx_model *ctx,
                                       uint8_t log2_size, uint8_t c_idx, int is_x)
{
    int ctx_base = is_x ? HEIC_CTX_LAST_SIG_COEFF_X_PREFIX
                        : HEIC_CTX_LAST_SIG_COEFF_Y_PREFIX;
    int ctx_offset, ctx_shift, max_prefix;
    uint32_t prefix = 0;

    if (c_idx == 0) {
        ctx_offset = 3 * ((int)log2_size - 2) + (((int)log2_size - 1) >> 2);
        ctx_shift = ((int)log2_size + 1) >> 2;
    } else {
        ctx_offset = 15;
        ctx_shift = (int)log2_size - 2;
    }
    max_prefix = ((int)log2_size << 1) - 1;
    while ((int)prefix < max_prefix) {
        int ctx_idx = ctx_base + ctx_offset + ((int)prefix >> ctx_shift);
        if (heic_cabac_decode_bin(cabac, &ctx[ctx_idx]) == 0) break;
        prefix++;
    }
    return prefix;
}

static int decode_last_sig_pos(heic_cabac *cabac, heic_ctx_model *ctx,
                               uint8_t log2_size, uint8_t c_idx,
                               uint32_t *out_x, uint32_t *out_y)
{
    uint32_t x_prefix = decode_last_sig_prefix(cabac, ctx, log2_size, c_idx, 1);
    uint32_t y_prefix = decode_last_sig_prefix(cabac, ctx, log2_size, c_idx, 0);
    uint32_t x, y;
    if (x_prefix > 3) {
        uint32_t n_bits = (x_prefix >> 1) - 1;
        uint32_t suffix = heic_cabac_decode_bypass_bits(cabac, (int)n_bits);
        x = ((2 + (x_prefix & 1)) << n_bits) + suffix;
    } else {
        x = x_prefix;
    }
    if (y_prefix > 3) {
        uint32_t n_bits = (y_prefix >> 1) - 1;
        uint32_t suffix = heic_cabac_decode_bypass_bits(cabac, (int)n_bits);
        y = ((2 + (y_prefix & 1)) << n_bits) + suffix;
    } else {
        y = y_prefix;
    }
    *out_x = x;
    *out_y = y;
    return 0;
}

static int decode_coded_sb_flag(heic_cabac *cabac, heic_ctx_model *ctx,
                                uint8_t c_idx, uint8_t csbf_neighbors)
{
    int csbf_ctx = csbf_neighbors != 0 ? 1 : 0;
    int ctx_idx = HEIC_CTX_CODED_SUB_BLOCK_FLAG + csbf_ctx + (c_idx > 0 ? 2 : 0);
    return heic_cabac_decode_bin(cabac, &ctx[ctx_idx]) != 0;
}

static int decode_greater1(heic_cabac *cabac, heic_ctx_model *ctx, uint8_t c_idx,
                           uint8_t ctx_set, uint8_t greater1_ctx)
{
    int ctx_idx = HEIC_CTX_COEFF_ABS_LEVEL_GREATER1
                + (c_idx > 0 ? 16 : 0)
                + (int)ctx_set * 4
                + (greater1_ctx < 3 ? greater1_ctx : 3);
    return heic_cabac_decode_bin(cabac, &ctx[ctx_idx]) != 0;
}

static int decode_greater2(heic_cabac *cabac, heic_ctx_model *ctx, uint8_t c_idx,
                           uint8_t ctx_set)
{
    int ctx_idx = HEIC_CTX_COEFF_ABS_LEVEL_GREATER2
                + (c_idx > 0 ? 4 : 0) + (int)ctx_set;
    return heic_cabac_decode_bin(cabac, &ctx[ctx_idx]) != 0;
}

static int32_t decode_abs_remaining(heic_cabac *cabac, uint8_t rice,
                                    int limited_prefix, int max_transform_range)
{
    uint32_t prefix = 0;
    uint32_t suffix;
    int32_t value;
    if (limited_prefix) {
        uint32_t longest = 32u - (3u + (uint32_t)max_transform_range) + 3u;
        while (prefix < longest && heic_cabac_decode_bypass(cabac) != 0)
            prefix++;
    } else {
        while (heic_cabac_decode_bypass(cabac) != 0 && prefix < 18)
            prefix++;
    }
    if ((!limited_prefix && prefix >= 18)
        || prefix - HEIC_MIN(prefix, 3) + rice > 31) {
        cabac->error = 1;
        return 0;
    }
    if (prefix <= 3) {
        suffix = rice > 0 ? heic_cabac_decode_bypass_bits(cabac, rice) : 0;
        value = (int32_t)((prefix << rice) + suffix);
    } else if (limited_prefix) {
        uint32_t max_prefix = 32u - (3u + (uint32_t)max_transform_range);
        uint32_t prefix_len = prefix - 3;
        uint32_t suffix_len =
            prefix_len == max_prefix
                ? (uint32_t)max_transform_range - rice
                : prefix_len;
        suffix = heic_cabac_decode_bypass_bits(cabac, (int)(suffix_len + rice));
        value = (int32_t)(suffix
            + ((((1u << prefix_len) - 1u) + 3u) << rice));
    } else {
        uint8_t suffix_bits = (uint8_t)(prefix - 3 + rice);
        suffix = heic_cabac_decode_bypass_bits(cabac, suffix_bits);
        uint32_t base = ((1u << (prefix - 3)) + 2u) << rice;
        value = (int32_t)(base + suffix);
    }
    return value;
}

int heic_decode_residual(heic_cabac *cabac, heic_ctx_model *ctx,
                         uint8_t log2_size, uint8_t c_idx, int scan_order,
                         int sign_data_hiding, int cu_transquant_bypass,
                         int transform_skip_enabled, uint8_t max_transform_skip_log2,
                         int transform_skip_context_enabled,
                         int implicit_rdpcm_enabled, int explicit_rdpcm_enabled,
                         int persistent_rice_adaptation_enabled,
                         int cabac_bypass_alignment_enabled,
                         int extended_precision_processing,
                         int max_transform_range,
                         uint8_t stat_coeff[4],
                         int pred_mode_intra, uint8_t intra_mode,
                         heic_coeff_buf *out, int *transform_skip, int *rdpcm_mode)
{
    uint32_t size, last_x, last_y, last_sb_idx, last_sb_x, last_sb_y;
    int sb_width, transform_skip_flag = 0, residual_dpcm = 0;
    int sig_ctx_override, sb_type;
    const uint8_t (*scan_sub)[2];
    const uint8_t (*scan_pos)[2];
    int scan_sub_n, scan_idx;
    uint8_t local_x, local_y, last_pos_in_sb;
    int coded_sb_flags[8][8];
    int prev_subblock_had_gt1 = 0;
    uint32_t sb_idx;

    if (!cabac || !ctx || !out || log2_size < 2 || log2_size > 5) return -1;
    size = 1u << log2_size;
    if (extended_precision_processing)
        memset(out->coeffs, 0, (size_t)size * size * sizeof(out->coeffs[0]));
    else
        memset(out->narrow, 0, (size_t)size * size * sizeof(out->narrow[0]));
    out->log2_size = log2_size;
    out->num_nonzero = 0;

    if (transform_skip_enabled && !cu_transquant_bypass
        && log2_size <= max_transform_skip_log2) {
        int ctx_idx = HEIC_CTX_TRANSFORM_SKIP_FLAG + (c_idx > 0 ? 1 : 0);
        transform_skip_flag = heic_cabac_decode_bin(cabac, &ctx[ctx_idx]) != 0;
    }
    if (transform_skip) *transform_skip = transform_skip_flag;
    if (!pred_mode_intra && explicit_rdpcm_enabled
        && (transform_skip_flag || cu_transquant_bypass)) {
        int ctx_off = c_idx > 0 ? 1 : 0;
        if (heic_cabac_decode_bin(cabac,
                &ctx[HEIC_CTX_EXPLICIT_RDPCM_FLAG + ctx_off])) {
            residual_dpcm = 1 + heic_cabac_decode_bin(cabac,
                &ctx[HEIC_CTX_EXPLICIT_RDPCM_DIR + ctx_off]);
        }
    } else if (pred_mode_intra && implicit_rdpcm_enabled
               && (transform_skip_flag || cu_transquant_bypass)) {
        if (intra_mode == 10) residual_dpcm = 1;
        else if (intra_mode == 26) residual_dpcm = 2;
    }
    if (rdpcm_mode) *rdpcm_mode = residual_dpcm;
    sig_ctx_override = transform_skip_context_enabled
                       && (transform_skip_flag || cu_transquant_bypass);
    sb_type = (c_idx == 0 ? 2 : 0)
              + ((transform_skip_flag || cu_transquant_bypass) ? 1 : 0);

    if (decode_last_sig_pos(cabac, ctx, log2_size, c_idx, &last_x, &last_y) != 0)
        return -1;

    if (scan_order == HEIC_SCAN_VERT) {
        uint32_t t = last_x;
        last_x = last_y;
        last_y = t;
    }
    if (last_x >= size || last_y >= size) return -1;

    get_scan_sub(log2_size, scan_order, &scan_sub, &scan_sub_n);
    scan_pos = scan_4x4(scan_order);
    scan_idx = scan_order;

    sb_width = (int)(size / 4);
    last_sb_x = last_x / 4;
    last_sb_y = last_y / 4;
    last_sb_idx = find_scan_pos(scan_sub, scan_sub_n, last_sb_x, last_sb_y);
    local_x = (uint8_t)(last_x % 4);
    local_y = (uint8_t)(last_y % 4);
    last_pos_in_sb = find_scan_pos_4x4(scan_pos, local_x, local_y);

    memset(coded_sb_flags, 0, sizeof(coded_sb_flags));

    sb_idx = last_sb_idx + 1;
    while (sb_idx > 0) {
        uint8_t sb_x, sb_y;
        sb_idx--;
        sb_x = scan_sub[sb_idx][0];
        sb_y = scan_sub[sb_idx][1];
        int right_coded, below_coded, csbf_neighbors, sb_coded, infer_sb_dc_sig;
        int prev_csbf;
        int sig_ctx_base, sig_ctx_offset;
        int sig_ctx_add, sig_dc_ctx;
        const uint8_t *sig_ctx_map;
        uint8_t start_pos, last_coeff;
        int32_t coeff_values[16];
        uint8_t sig_positions[16];
        uint8_t needs_remaining[16];
        int n_sig = 0;
        int can_infer_dc;
        int n;
        int base_cs, ctx_set, this_subblock_had_gt1;
        uint8_t greater1_ctx, last_greater1_flag;
        int first_g1_idx;
        int max_g1;
        uint8_t first_sig_pos, last_sig_pos;
        int sign_hidden;
        int n_signs;
        uint32_t sign_bits;
        uint32_t sign_mask;
        int i;
        uint8_t rice_param;
        int32_t sum_abs_level;
        int first_remaining;

        right_coded = ((int)sb_x + 1) < sb_width
                          ? coded_sb_flags[sb_y][sb_x + 1]
                          : 0;
        below_coded = ((int)sb_y + 1) < sb_width
                          ? coded_sb_flags[sb_y + 1][sb_x]
                          : 0;
        csbf_neighbors = (right_coded ? 1 : 0) | (below_coded ? 2 : 0);

        if (sb_idx > 0 && sb_idx < last_sb_idx) {
            sb_coded = decode_coded_sb_flag(cabac, ctx, c_idx, (uint8_t)csbf_neighbors);
            infer_sb_dc_sig = sb_coded;
        } else {
            sb_coded = 1;
            infer_sb_dc_sig = 0;
        }
        if (sb_coded) coded_sb_flags[sb_y][sb_x] = 1;
        prev_csbf = csbf_neighbors;
        if (!sb_coded) continue;

        sig_ctx_base = HEIC_CTX_SIG_COEFF_FLAG + (c_idx > 0 ? 27 : 0);
        if (sb_width == 1) {
            sig_ctx_offset = 0;
        } else if (c_idx == 0) {
            sig_ctx_offset = (sb_x + sb_y > 0 ? 3 : 0)
                           + (sb_width == 2 ? (scan_idx == 0 ? 9 : 15) : 21);
        } else {
            sig_ctx_offset = sb_width == 2 ? 9 : 12;
        }
        if (sb_width == 1) {
            sig_ctx_map = HEIC_SIG_CTX_4X4[scan_idx];
            sig_ctx_add = sig_ctx_base;
            sig_dc_ctx = sig_ctx_base + sig_ctx_map[0];
        } else {
            sig_ctx_map = HEIC_SIG_CTX_LOCAL[scan_idx][prev_csbf];
            sig_ctx_add = sig_ctx_base + sig_ctx_offset;
            sig_dc_ctx = (sb_x == 0 && sb_y == 0)
                              ? sig_ctx_base
                              : sig_ctx_add + sig_ctx_map[0];
        }

        start_pos = (sb_idx == last_sb_idx) ? last_pos_in_sb : 15;
        can_infer_dc = infer_sb_dc_sig;

        if (sb_idx == last_sb_idx) {
            sig_positions[n_sig++] = start_pos;
            can_infer_dc = 0;
            last_coeff = start_pos > 0 ? (uint8_t)(start_pos - 1) : 0;
        } else {
            last_coeff = 15;
        }

        if (!(sb_idx == last_sb_idx && start_pos == 0)) {
            for (n = (int)last_coeff; n >= 1; n--) {
                int sig_ctx = sig_ctx_override
                                  ? HEIC_CTX_SIG_COEFF_FLAG + (c_idx == 0 ? 42 : 43)
                                  : sig_ctx_add + sig_ctx_map[n];
                int sig = heic_cabac_decode_bin(cabac, &ctx[sig_ctx]) != 0;
                if (sig) {
                    sig_positions[n_sig++] = (uint8_t)n;
                    can_infer_dc = 0;
                }
            }
        }

        if (start_pos > 0) {
            if (can_infer_dc) {
                sig_positions[n_sig++] = 0;
            } else {
                int sig_ctx = sig_ctx_override
                                  ? HEIC_CTX_SIG_COEFF_FLAG + (c_idx == 0 ? 42 : 43)
                                  : sig_dc_ctx;
                int sig = heic_cabac_decode_bin(cabac, &ctx[sig_ctx]) != 0;
                if (sig) sig_positions[n_sig++] = 0;
            }
        }

        if (n_sig == 0) continue;

        base_cs = (sb_idx == 0 || c_idx > 0) ? 0 : 2;
        ctx_set = base_cs + (prev_subblock_had_gt1 ? 1 : 0);
        this_subblock_had_gt1 = 0;
        greater1_ctx = 1;
        last_greater1_flag = 0;
        first_g1_idx = -1;
        max_g1 = n_sig < 8 ? n_sig : 8;

        for (i = 0; i < max_g1; i++) {
            int g1;
            coeff_values[i] = 1;
            needs_remaining[i] = 0;
            if (i > 0 && greater1_ctx > 0) {
                if (last_greater1_flag)
                    greater1_ctx = 0;
                else
                    greater1_ctx++;
            }
            g1 = decode_greater1(cabac, ctx, c_idx, (uint8_t)ctx_set, greater1_ctx);
            last_greater1_flag = (uint8_t)g1;
            if (g1) {
                coeff_values[i] = 2;
                this_subblock_had_gt1 = 1;
                if (first_g1_idx < 0)
                    first_g1_idx = i;
                else
                    needs_remaining[i] = 1;
            }
        }
        for (; i < n_sig; i++) {
            coeff_values[i] = 1;
            needs_remaining[i] = 1;
        }

        if (first_g1_idx >= 0) {
            if (decode_greater2(cabac, ctx, c_idx, (uint8_t)ctx_set)) {
                coeff_values[first_g1_idx] = 3;
                needs_remaining[first_g1_idx] = 1;
            }
        }

        if (cabac_bypass_alignment_enabled) {
            int has_remaining = 0;
            for (i = 0; i < n_sig; i++)
                has_remaining |= needs_remaining[i];
            if (has_remaining) heic_cabac_align_bypass(cabac);
        }

        /* Highest scan index first in array, lowest last. */
        last_sig_pos = sig_positions[0];
        first_sig_pos = sig_positions[n_sig - 1];
        sign_hidden = sign_data_hiding && !cu_transquant_bypass && !residual_dpcm
                      && (last_sig_pos - first_sig_pos) > 3;

        n_signs = n_sig - (sign_hidden ? 1 : 0);
        sign_bits = heic_cabac_decode_bypass_bits(cabac, n_signs);
        sign_mask = n_signs > 0 ? 1u << (n_signs - 1) : 0;

        rice_param = persistent_rice_adaptation_enabled && stat_coeff
                         ? (uint8_t)(stat_coeff[sb_type] / 4) : 0;
        first_remaining = 1;
        sum_abs_level = 0;
        out->num_nonzero = (uint16_t)(out->num_nonzero + n_sig);
        for (i = 0; i < n_sig; i++) {
            int pos = sig_positions[i];
            int32_t v = coeff_values[i];
            if (needs_remaining[i]) {
                uint8_t old_rice = rice_param;
                int32_t rem = decode_abs_remaining(
                    cabac, rice_param, extended_precision_processing,
                    max_transform_range);
                int32_t sum = (int32_t)v + (int32_t)rem;
                if ((uint32_t)sum > 3u * (1u << old_rice)) {
                    uint8_t max_rice =
                        persistent_rice_adaptation_enabled ? 13 : 4;
                    if (rice_param < max_rice) rice_param++;
                }
                if (persistent_rice_adaptation_enabled && stat_coeff
                    && first_remaining) {
                    uint8_t stat = stat_coeff[sb_type];
                    uint8_t stat_rice = (uint8_t)(stat / 4);
                    if ((uint32_t)rem >= 3u * (1u << stat_rice)) {
                        if (stat < 55) stat_coeff[sb_type] = (uint8_t)(stat + 1);
                    } else if ((uint32_t)rem * 2u < (1u << stat_rice)
                               && stat > 0) {
                        stat_coeff[sb_type] = (uint8_t)(stat - 1);
                    }
                    first_remaining = 0;
                }
                if (sum > (1 << max_transform_range) - 1)
                    sum = (1 << max_transform_range) - 1;
                v = sum;
            }
            if (sign_bits & sign_mask)
                v = -v;
            sign_mask >>= 1;
            sum_abs_level += v;
            if (i == n_sig - 1 && sign_hidden && (sum_abs_level & 1) != 0)
                v = -v;
            {
                int x = (int)sb_x * 4 + scan_pos[pos][0];
                int y = (int)sb_y * 4 + scan_pos[pos][1];
                if (extended_precision_processing)
                    out->coeffs[y * (int)size + x] = v;
                else
                    out->narrow[y * (int)size + x] = (int16_t)v;
            }
        }

        prev_subblock_had_gt1 = this_subblock_had_gt1;
    }

    return cabac->error ? -1 : 0;
}
