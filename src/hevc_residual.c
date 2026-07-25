/* hevc_residual.c -- residual_coding CABAC path (port of imazen/heic residual.rs) */
#include "heic_internal.h"

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

static const uint8_t HEIC_CTX_IDX_MAP_4X4[16] = {
    0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8
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

static uint32_t find_scan_pos(const uint8_t (*scan)[2], int n, uint32_t x, uint32_t y)
{
    int i;
    for (i = 0; i < n; i++)
        if (scan[i][0] == x && scan[i][1] == y) return (uint32_t)i;
    return 0;
}

static uint8_t find_scan_pos_4x4(const uint8_t (*scan)[2], uint8_t x, uint8_t y)
{
    int i;
    for (i = 0; i < 16; i++)
        if (scan[i][0] == x && scan[i][1] == y) return (uint8_t)i;
    return 0;
}

static void coeff_set(heic_coeff_buf *b, int x, int y, int16_t v)
{
    int stride = 1 << b->log2_size;
    b->coeffs[y * stride + x] = v;
    if (v != 0) {
        if (b->num_nonzero < 0xFFFFu) b->num_nonzero++;
    }
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

static int calc_sig_ctx(uint8_t x_c, uint8_t y_c, uint8_t log2_size, uint8_t c_idx,
                        uint8_t scan_idx, uint8_t prev_csbf)
{
    uint8_t sb_width = (uint8_t)(1u << (log2_size - 2));
    int sig_ctx;
    if (sb_width == 1) {
        sig_ctx = HEIC_CTX_IDX_MAP_4X4[(y_c * 4 + x_c) & 15];
    } else if (x_c == 0 && y_c == 0) {
        sig_ctx = 0;
    } else {
        uint8_t x_s = x_c >> 2, y_s = y_c >> 2;
        uint8_t x_p = x_c & 3, y_p = y_c & 3;
        int ctx;
        switch (prev_csbf) {
        case 0:
            if (x_p + y_p >= 3) ctx = 0;
            else if (x_p + y_p > 0) ctx = 1;
            else ctx = 2;
            break;
        case 1:
            if (y_p == 0) ctx = 2;
            else if (y_p == 1) ctx = 1;
            else ctx = 0;
            break;
        case 2:
            if (x_p == 0) ctx = 2;
            else if (x_p == 1) ctx = 1;
            else ctx = 0;
            break;
        default:
            ctx = 2;
            break;
        }
        if (c_idx == 0) {
            if (x_s + y_s > 0) ctx += 3;
            if (sb_width == 2)
                ctx += (scan_idx == 0) ? 9 : 15;
            else
                ctx += 21;
        } else {
            if (sb_width == 2) ctx += 9;
            else ctx += 12;
        }
        sig_ctx = ctx;
    }
    return HEIC_CTX_SIG_COEFF_FLAG + (c_idx > 0 ? 27 : 0) + sig_ctx;
}

static int decode_sig_flag(heic_cabac *cabac, heic_ctx_model *ctx, uint8_t c_idx,
                           uint8_t pos, uint8_t log2_size, uint8_t scan_idx,
                           uint8_t sb_x, uint8_t sb_y, uint8_t prev_csbf,
                           const uint8_t (*scan_pos)[2])
{
    uint8_t x_c = (uint8_t)(sb_x * 4 + scan_pos[pos][0]);
    uint8_t y_c = (uint8_t)(sb_y * 4 + scan_pos[pos][1]);
    int ctx_idx = calc_sig_ctx(x_c, y_c, log2_size, c_idx, scan_idx, prev_csbf);
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

static int16_t decode_abs_remaining(heic_cabac *cabac, uint8_t *rice_param,
                                    int16_t base_level)
{
    uint32_t prefix = 0;
    int16_t value;
    uint8_t rice = *rice_param;
    uint32_t threshold;
    while (heic_cabac_decode_bypass(cabac) != 0 && prefix < 24)
        prefix++;
    if (prefix <= 3) {
        uint32_t suffix = rice > 0 ? heic_cabac_decode_bypass_bits(cabac, rice) : 0;
        value = (int16_t)((prefix << rice) + suffix);
    } else {
        uint8_t suffix_bits = (uint8_t)(prefix - 3 + rice);
        uint32_t suffix = heic_cabac_decode_bypass_bits(cabac, suffix_bits);
        uint32_t base = ((1u << (prefix - 3)) + 2u) << rice;
        value = (int16_t)(base + suffix);
    }
    threshold = 3u * (1u << rice);
    if ((uint32_t)(base_level < 0 ? -base_level : base_level)
          + (uint32_t)(value < 0 ? -value : value)
        > threshold) {
        if (rice < 4) rice++;
    }
    *rice_param = rice;
    return value;
}

int heic_decode_residual(heic_cabac *cabac, heic_ctx_model *ctx,
                         uint8_t log2_size, uint8_t c_idx, int scan_order,
                         int sign_data_hiding, int cu_transquant_bypass,
                         int transform_skip_enabled,
                         heic_coeff_buf *out, int *transform_skip)
{
    uint32_t size, last_x, last_y, last_sb_idx, last_sb_x, last_sb_y;
    int sb_width, transform_skip_flag = 0;
    const uint8_t (*scan_sub)[2];
    const uint8_t (*scan_pos)[2];
    int scan_sub_n, scan_idx;
    uint8_t local_x, local_y, last_pos_in_sb;
    int coded_sb_flags[8][8];
    int prev_subblock_had_gt1 = 0;
    uint32_t sb_idx;

    if (!cabac || !ctx || !out || log2_size < 2 || log2_size > 5) return -1;
    memset(out, 0, sizeof(*out));
    out->log2_size = log2_size;
    size = 1u << log2_size;

    if (transform_skip_enabled && !cu_transquant_bypass && log2_size <= 2) {
        int ctx_idx = HEIC_CTX_TRANSFORM_SKIP_FLAG + (c_idx > 0 ? 1 : 0);
        transform_skip_flag = heic_cabac_decode_bin(cabac, &ctx[ctx_idx]) != 0;
    }
    if (transform_skip) *transform_skip = transform_skip_flag;

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
        uint8_t start_pos, last_coeff;
        int16_t coeff_values[16];
        int coeff_flags[16];
        uint8_t num_coeffs = 0;
        int can_infer_dc;
        int n;
        int base_cs, ctx_set, this_subblock_had_gt1;
        uint8_t greater1_ctx, last_greater1_flag;
        int first_g1_idx;
        int g1_positions[16];
        int needs_remaining[16];
        int max_g1, g1_count;
        uint8_t first_sig_pos, last_sig_pos;
        int sign_hidden;
        uint8_t sig_positions[16];
        int n_sig, i;
        uint8_t coeff_signs[16];
        uint8_t rice_param;
        int32_t sum_abs_level;

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

        start_pos = (sb_idx == last_sb_idx) ? last_pos_in_sb : 15;
        memset(coeff_values, 0, sizeof(coeff_values));
        memset(coeff_flags, 0, sizeof(coeff_flags));
        can_infer_dc = infer_sb_dc_sig;

        if (sb_idx == last_sb_idx) {
            coeff_flags[start_pos] = 1;
            coeff_values[start_pos] = 1;
            num_coeffs = 1;
            can_infer_dc = 0;
            last_coeff = start_pos > 0 ? (uint8_t)(start_pos - 1) : 0;
        } else {
            last_coeff = 15;
        }

        if (!(sb_idx == last_sb_idx && start_pos == 0)) {
            for (n = (int)last_coeff; n >= 1; n--) {
                int sig = decode_sig_flag(cabac, ctx, c_idx, (uint8_t)n, log2_size,
                                          (uint8_t)scan_idx, sb_x, sb_y,
                                          (uint8_t)prev_csbf, scan_pos);
                if (sig) {
                    coeff_flags[n] = 1;
                    coeff_values[n] = 1;
                    num_coeffs++;
                    can_infer_dc = 0;
                }
            }
        }

        if (start_pos > 0) {
            if (can_infer_dc) {
                coeff_flags[0] = 1;
                coeff_values[0] = 1;
                num_coeffs++;
            } else {
                int sig = decode_sig_flag(cabac, ctx, c_idx, 0, log2_size,
                                          (uint8_t)scan_idx, sb_x, sb_y,
                                          (uint8_t)prev_csbf, scan_pos);
                if (sig) {
                    coeff_flags[0] = 1;
                    coeff_values[0] = 1;
                    num_coeffs++;
                }
            }
        }

        if (num_coeffs == 0) continue;

        base_cs = (sb_idx == 0 || c_idx > 0) ? 0 : 2;
        ctx_set = base_cs + (prev_subblock_had_gt1 ? 1 : 0);
        this_subblock_had_gt1 = 0;
        greater1_ctx = 1;
        last_greater1_flag = 0;
        first_g1_idx = -1;
        memset(g1_positions, 0, sizeof(g1_positions));
        memset(needs_remaining, 0, sizeof(needs_remaining));
        max_g1 = num_coeffs < 8 ? num_coeffs : 8;
        g1_count = 0;

        for (n = (int)start_pos; n >= 0; n--) {
            int g1;
            if (!coeff_flags[n]) continue;
            if (g1_count >= max_g1) {
                needs_remaining[n] = 1;
                continue;
            }
            if (g1_count > 0 && greater1_ctx > 0) {
                if (last_greater1_flag)
                    greater1_ctx = 0;
                else
                    greater1_ctx++;
            }
            g1 = decode_greater1(cabac, ctx, c_idx, (uint8_t)ctx_set, greater1_ctx);
            last_greater1_flag = (uint8_t)g1;
            if (g1) {
                coeff_values[n] = 2;
                g1_positions[n] = 1;
                this_subblock_had_gt1 = 1;
                if (first_g1_idx < 0)
                    first_g1_idx = n;
                else
                    needs_remaining[n] = 1;
            }
            g1_count++;
        }

        if (first_g1_idx >= 0) {
            if (decode_greater2(cabac, ctx, c_idx, (uint8_t)ctx_set)) {
                coeff_values[first_g1_idx] = 3;
                needs_remaining[first_g1_idx] = 1;
            }
        }

        /* One reverse pass: sig list high→low (matches prior order) + first/last. */
        n_sig = 0;
        for (n = (int)start_pos; n >= 0; n--) {
            if (coeff_flags[n]) sig_positions[n_sig++] = (uint8_t)n;
        }
        if (n_sig == 0) {
            prev_subblock_had_gt1 = this_subblock_had_gt1;
            continue;
        }
        /* Highest scan index first in array, lowest last. */
        last_sig_pos = sig_positions[0];
        first_sig_pos = sig_positions[n_sig - 1];
        sign_hidden = sign_data_hiding && !cu_transquant_bypass
                      && (last_sig_pos - first_sig_pos) > 3;

        memset(coeff_signs, 0, sizeof(coeff_signs));
        for (i = 0; i < n_sig - 1; i++)
            coeff_signs[i] = (uint8_t)heic_cabac_decode_bypass(cabac);
        if (!sign_hidden)
            coeff_signs[n_sig - 1] = (uint8_t)heic_cabac_decode_bypass(cabac);

        rice_param = 0;
        sum_abs_level = 0;
        for (i = 0; i < n_sig; i++) {
            int pos = sig_positions[i];
            int16_t v = coeff_values[pos];
            if (needs_remaining[pos]) {
                int16_t rem = decode_abs_remaining(cabac, &rice_param, v);
                int32_t sum = (int32_t)v + (int32_t)rem;
                if (sum > 32767) sum = 32767;
                v = (int16_t)sum;
            }
            if (coeff_signs[i]) v = (int16_t)(-v);
            sum_abs_level += v;
            if (i == n_sig - 1 && sign_hidden && (sum_abs_level & 1) != 0)
                v = (int16_t)(-v);
            coeff_values[pos] = v;
            {
                int x = (int)sb_x * 4 + scan_pos[pos][0];
                int y = (int)sb_y * 4 + scan_pos[pos][1];
                coeff_set(out, x, y, v);
            }
        }

        prev_subblock_had_gt1 = this_subblock_had_gt1;
    }

    return cabac->error ? -1 : 0;
}
