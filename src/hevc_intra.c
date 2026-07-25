/* hevc_intra.c -- intra prediction (port of imazen/heic intra.rs) */
#include "heic_internal.h"

#define HEIC_MAX_INTRA 32
#define HEIC_BORDER_N  (4 * HEIC_MAX_INTRA + 1)

static const int16_t HEIC_INTRA_ANGLE[35] = {
    0, 0,
    32, 26, 21, 17, 13, 9, 5, 2,
    0,
    -2, -5, -9, -13, -17, -21, -26,
    -32,
    -26, -21, -17, -13, -9, -5, -2,
    0,
    2, 5, 9, 13, 17, 21, 26,
    32,
};
static const int32_t HEIC_INV_ANGLE[15] = {
    -4096, -1638, -910, -630, -482, -390, -315,
    -256,
    -315, -390, -482, -630, -910, -1638, -4096,
};

void heic_fill_mpm(uint8_t cand_a, uint8_t cand_b, uint8_t mpm[3])
{
    if (cand_a == cand_b) {
        if (cand_a < 2) {
            mpm[0] = 0;
            mpm[1] = 1;
            mpm[2] = 26;
        } else {
            uint8_t mode = cand_a;
            uint8_t left = (uint8_t)(2 + ((mode - 2 + 31) % 32));
            uint8_t right = (uint8_t)(2 + ((mode - 2 + 1) % 32));
            mpm[0] = cand_a;
            mpm[1] = left;
            mpm[2] = right;
        }
    } else {
        uint8_t third;
        if (cand_a != 0 && cand_b != 0) third = 0;
        else if (cand_a != 1 && cand_b != 1) third = 1;
        else third = 26;
        mpm[0] = cand_a;
        mpm[1] = cand_b;
        mpm[2] = third;
    }
}

static int32_t inv_angle(uint8_t mode)
{
    if (mode >= 11 && mode <= 25) return HEIC_INV_ANGLE[mode - 11];
    return 0;
}

/* Predict writes always land inside the PB (caller guarantees). */
static inline void plane_put(uint16_t *p, int stride, int x, int y, uint16_t v)
{
    p[(size_t)y * (size_t)stride + (size_t)x] = v;
}

static void ref_subst(int32_t *border, const int *avail, int center, int size,
                      int32_t def)
{
    int first_val = def, found = 0, current, i, idx;
    for (i = (int)(2 * size) - 1; i >= 0; i--) {
        idx = center - 1 - i;
        if (avail[idx]) {
            first_val = border[idx];
            found = 1;
            break;
        }
    }
    if (!found && avail[center]) {
        first_val = border[center];
        found = 1;
    }
    if (!found) {
        for (i = 0; i < 2 * size; i++) {
            idx = center + 1 + i;
            if (avail[idx]) {
                first_val = border[idx];
                break;
            }
        }
    }
    current = first_val;
    for (i = (int)(2 * size) - 1; i >= 0; i--) {
        idx = center - 1 - i;
        if (avail[idx]) current = border[idx];
        else border[idx] = current;
    }
    if (avail[center]) current = border[center];
    else border[center] = current;
    for (i = 0; i < 2 * size; i++) {
        idx = center + 1 + i;
        if (avail[idx]) current = border[idx];
        else border[idx] = current;
    }
}

static void fill_border(heic_frame *frame, uint32_t x, uint32_t y, uint32_t size,
                        uint8_t c_idx, int32_t *border, int center)
{
    uint16_t *plane;
    int stride, plane_n, frame_w, frame_h, avail_left, avail_top, avail_tl;
    int avail[HEIC_BORDER_N];
    uint32_t avail_count = 0, total = 4 * size + 1;
    int32_t def;
    int corner_ok = 0;
    uint32_t i;

    if (c_idx == 0) {
        plane = frame->y;
        stride = frame->y_stride;
        plane_n = frame->width * frame->height;
        frame_w = frame->width;
        frame_h = frame->height;
    } else {
        plane = (c_idx == 1) ? frame->cb : frame->cr;
        stride = frame->c_stride;
        plane_n = frame->c_width * frame->c_height;
        frame_w = frame->c_width;
        frame_h = frame->c_height;
    }
    if (!plane) {
        def = 1 << (frame->bit_depth - 1);
        for (i = 0; i < total; i++) border[center - 2 * (int)size + (int)i] = def;
        return;
    }
    def = 1 << (frame->bit_depth - 1);
    avail_left = x > 0;
    avail_top = y > 0;
    avail_tl = avail_left && avail_top;

    /* Only clear the slots we may write (4*size+1 around center). */
    {
        int span = (int)(4 * size + 1);
        int base = center - 2 * (int)size;
        memset(avail + base, 0, (size_t)span * sizeof(int));
    }

    /* Corner: direct loads when in-bounds (avoid plane_get). */
    if (avail_tl) {
        uint16_t raw =
            plane[(size_t)(y - 1) * (size_t)stride + (size_t)(x - 1)];
        if (raw != HEIC_UNINIT_SAMPLE) {
            border[center] = raw;
            corner_ok = 1;
            avail_count++;
        }
    }
    if (!corner_ok && avail_top) {
        uint16_t raw = plane[(size_t)(y - 1) * (size_t)stride + (size_t)x];
        if (raw != HEIC_UNINIT_SAMPLE) {
            border[center] = raw;
            corner_ok = 1;
            avail_count++;
        }
    }
    if (!corner_ok && avail_left) {
        uint16_t raw = plane[(size_t)y * (size_t)stride + (size_t)(x - 1)];
        if (raw != HEIC_UNINIT_SAMPLE) {
            border[center] = raw;
            corner_ok = 1;
            avail_count++;
        }
    }
    if (!corner_ok) border[center] = def;
    avail[center] = corner_ok;

    if (avail_top) {
        uint32_t top_count = 2 * size;
        uint32_t n_avail = size; /* first N top samples always reconstructed */
        const uint16_t *top_row;
        if (top_count > (uint32_t)frame_w - x) top_count = (uint32_t)frame_w - x;
        if (n_avail > top_count) n_avail = top_count;
        top_row = plane + (size_t)(y - 1) * (size_t)stride + (size_t)x;
        if (!heic_simd_u16_to_i32_avail(top_row, &border[center + 1],
                                        &avail[center + 1], (int)n_avail)) {
            for (i = 0; i < n_avail; i++) {
                border[center + 1 + (int)i] = top_row[i];
                avail[center + 1 + (int)i] = 1;
            }
        }
        avail_count += n_avail;
        /* Extension N..2N may land on not-yet-decoded CUs (UNINIT). */
        if (top_count > n_avail) {
            if (!heic_simd_border_top_ext(top_row + n_avail, &border[center + 1 + (int)n_avail],
                                          &avail[center + 1 + (int)n_avail],
                                          (int)(top_count - n_avail))) {
                for (i = n_avail; i < top_count; i++) {
                    uint16_t raw = top_row[i];
                    if (raw != HEIC_UNINIT_SAMPLE) {
                        int idx = center + 1 + (int)i;
                        border[idx] = raw;
                        avail[idx] = 1;
                        avail_count++;
                    }
                }
            } else {
                /* Count valids for avail_count bookkeeping */
                for (i = n_avail; i < top_count; i++)
                    if (avail[center + 1 + (int)i]) avail_count++;
            }
        }
    }
    if (avail_left) {
        uint32_t left_count = 2 * size;
        uint32_t n_avail = size; /* first N left samples always reconstructed */
        const uint16_t *left_p;
        if (left_count > (uint32_t)frame_h - y) left_count = (uint32_t)frame_h - y;
        if (n_avail > left_count) n_avail = left_count;
        left_p = plane + (size_t)y * (size_t)stride + (size_t)(x - 1);
        for (i = 0; i < n_avail; i++) {
            border[center - 1 - (int)i] = left_p[(size_t)i * (size_t)stride];
            avail[center - 1 - (int)i] = 1;
            avail_count++;
        }
        for (i = n_avail; i < left_count; i++) {
            uint16_t raw = left_p[(size_t)i * (size_t)stride];
            if (raw != HEIC_UNINIT_SAMPLE) {
                int idx = center - 1 - (int)i;
                border[idx] = raw;
                avail[idx] = 1;
                avail_count++;
            }
        }
    }
    if (avail_count < total)
        ref_subst(border, avail, center, (int)size, def);
    (void)plane_n;
}

static void sample_filter(int32_t *border, int center, int n_t, uint8_t c_idx,
                          uint8_t mode, int strong, int bit_depth)
{
    int filter_flag, bi_int, i;
    int32_t pf[HEIC_BORDER_N];
    int pfc = 2 * HEIC_MAX_INTRA;

    if (mode == 1 || n_t == 4) filter_flag = 0;
    else {
        int mdvh = abs((int)mode - 26);
        int mdhh = abs((int)mode - 10);
        int min_d = mdvh < mdhh ? mdvh : mdhh;
        if (n_t == 8) filter_flag = min_d > 7;
        else if (n_t == 16) filter_flag = min_d > 1;
        else if (n_t == 32) filter_flag = min_d > 0;
        else filter_flag = 0;
    }
    if (!filter_flag) return;

    bi_int = strong && c_idx == 0 && n_t == 32
             && abs(border[center] + border[center + 64] - 2 * border[center + 32])
                    < (1 << (bit_depth - 5))
             && abs(border[center] + border[center - 64] - 2 * border[center - 32])
                    < (1 << (bit_depth - 5));

    if (bi_int) {
        int32_t p0 = border[center];
        int32_t p_neg = border[center - 64];
        int32_t p_pos = border[center + 64];
        pf[pfc - 2 * n_t] = border[center - 2 * n_t];
        pf[pfc + 2 * n_t] = border[center + 2 * n_t];
        pf[pfc] = border[center];
        for (i = 1; i < 64; i++) {
            pf[pfc - i] = p0 + ((i * (p_neg - p0) + 32) >> 6);
            pf[pfc + i] = p0 + ((i * (p_pos - p0) + 32) >> 6);
        }
    } else {
        pf[pfc - 2 * n_t] = border[center - 2 * n_t];
        pf[pfc + 2 * n_t] = border[center + 2 * n_t];
        for (i = -(2 * n_t - 1); i <= (2 * n_t - 1); i++) {
            int idx = center + i;
            pf[pfc + i] = (border[idx + 1] + 2 * border[idx] + border[idx - 1] + 2) >> 2;
        }
    }
    for (i = 0; i <= 4 * n_t; i++)
        border[center - 2 * n_t + i] = pf[pfc - 2 * n_t + i];
}

static void predict_planar(uint16_t *plane, int stride, uint32_t x, uint32_t y,
                           uint32_t size, uint8_t log2_size, int max_val,
                           const int32_t *border, int center)
{
    int n = (int)size, px, py;
    int32_t right = border[center + 1 + n];
    int32_t bottom = border[center - 1 - n];
    for (py = 0; py < n; py++) {
        int32_t left = border[center - 1 - py];
        uint16_t *dst = plane + ((size_t)y + (size_t)py) * (size_t)stride + (size_t)x;
        for (px = 0; px < n; px++) {
            int32_t top = border[center + 1 + px];
            int32_t pred = ((n - 1 - px) * left + (px + 1) * right
                            + (n - 1 - py) * top + (py + 1) * bottom + n)
                           >> (log2_size + 1);
            if (pred < 0) pred = 0;
            if (pred > max_val) pred = max_val;
            dst[px] = (uint16_t)pred;
        }
    }
}

static void predict_dc(uint16_t *plane, int stride, uint32_t x, uint32_t y,
                       uint32_t size, uint8_t log2_size, uint8_t c_idx, int max_val,
                       const int32_t *border, int center)
{
    int n = (int)size;
    int32_t dc = 0;
    int i, px, py;
    uint16_t dcu;
    for (i = 0; i < n; i++) {
        dc += border[center + 1 + i];
        dc += border[center - 1 - i];
    }
    dc = (dc + n) >> (log2_size + 1);
    if (dc < 0) dc = 0;
    if (dc > max_val) dc = max_val;
    dcu = (uint16_t)dc;

    if (c_idx == 0 && size < 32) {
        int32_t corner = (border[center - 1] + 2 * dc + border[center + 1] + 2) >> 2;
        if (corner < 0) corner = 0;
        if (corner > max_val) corner = max_val;
        plane_put(plane, stride, (int)x, (int)y, (uint16_t)corner);
        for (px = 1; px < n; px++) {
            int32_t pred = (border[center + 1 + px] + 3 * dc + 2) >> 2;
            if (pred < 0) pred = 0;
            if (pred > max_val) pred = max_val;
            plane_put(plane, stride, (int)x + px, (int)y, (uint16_t)pred);
        }
        for (py = 1; py < n; py++) {
            int32_t pred = (border[center - 1 - py] + 3 * dc + 2) >> 2;
            if (pred < 0) pred = 0;
            if (pred > max_val) pred = max_val;
            plane_put(plane, stride, (int)x, (int)y + py, (uint16_t)pred);
        }
        for (py = 1; py < n; py++) {
            uint16_t *dst =
                plane + ((size_t)y + (size_t)py) * (size_t)stride + (size_t)x + 1;
            for (px = 1; px < n; px++) dst[px - 1] = dcu;
        }
    } else {
        for (py = 0; py < n; py++) {
            uint16_t *dst =
                plane + ((size_t)y + (size_t)py) * (size_t)stride + (size_t)x;
            for (px = 0; px < n; px++) dst[px] = dcu;
        }
    }
}

static void predict_angular(uint16_t *plane, int stride, uint32_t x, uint32_t y,
                            uint32_t size, uint8_t c_idx, uint8_t mode, int max_val,
                            const int32_t *border, int center)
{
    int n = (int)size;
    int32_t angle = HEIC_INTRA_ANGLE[mode];
    int32_t ref_arr[HEIC_BORDER_N];
    int rc = 2 * HEIC_MAX_INTRA;
    int px, py;

    /* Only the used slice of ref_arr is written below; skip full memset. */
    if (mode >= 18) {
        int i;
        for (i = 0; i <= n; i++)
            ref_arr[rc + i] = border[center + i];
        if (angle < 0) {
            int32_t inv = inv_angle(mode);
            int32_t ext = (n * angle) >> 5;
            int32_t xx;
            if (ext < -1) {
                for (xx = ext; xx <= -1; xx++) {
                    int32_t idx = (xx * inv + 128) >> 8;
                    if (idx >= 0 && idx <= 2 * n)
                        ref_arr[rc + (int)xx] = border[center - (int)idx];
                }
            }
        } else {
            for (i = 0; i < n; i++)
                ref_arr[rc + n + 1 + i] = border[center + n + 1 + i];
        }
        for (py = 0; py < n; py++) {
            int32_t i_idx = ((py + 1) * angle) >> 5;
            int32_t i_fact = ((py + 1) * angle) & 31;
            int base = rc + (int)i_idx + 1;
            uint16_t *dst =
                plane + ((size_t)y + (size_t)py) * (size_t)stride + (size_t)x;
            if (i_fact != 0 &&
                heic_simd_intra_ang_row(dst, &ref_arr[base], n, 32 - (int)i_fact,
                                        (int)i_fact, max_val))
                continue;
            for (px = 0; px < n; px++) {
                int32_t pred;
                if (i_fact != 0)
                    pred = ((32 - i_fact) * ref_arr[base + px]
                            + i_fact * ref_arr[base + px + 1] + 16)
                           >> 5;
                else
                    pred = ref_arr[base + px];
                if (pred < 0) pred = 0;
                if (pred > max_val) pred = max_val;
                dst[px] = (uint16_t)pred;
            }
        }
        if (mode == 26 && c_idx == 0 && size < 32) {
            for (py = 0; py < n; py++) {
                int32_t pred = border[center + 1]
                               + ((border[center - 1 - py] - border[center]) >> 1);
                if (pred < 0) pred = 0;
                if (pred > max_val) pred = max_val;
                plane_put(plane, stride, (int)x, (int)y + py, (uint16_t)pred);
            }
        }
    } else {
        int i;
        for (i = 0; i <= n; i++)
            ref_arr[rc + i] = border[center - i];
        if (angle < 0) {
            int32_t inv = inv_angle(mode);
            int32_t ext = (n * angle) >> 5;
            int32_t xx;
            if (ext < -1) {
                for (xx = ext; xx <= -1; xx++) {
                    int32_t idx = (xx * inv + 128) >> 8;
                    if (idx >= 0 && idx <= 2 * n)
                        ref_arr[rc + (int)xx] = border[center + (int)idx];
                }
            }
        } else {
            for (i = n + 1; i <= 2 * n; i++)
                ref_arr[rc + i] = border[center - i];
        }
        for (py = 0; py < n; py++) {
            int row_base = rc + py + 1;
            uint16_t *dst =
                plane + ((size_t)y + (size_t)py) * (size_t)stride + (size_t)x;
            if (heic_simd_intra_ang_row_var(dst, ref_arr, n, row_base, angle, max_val))
                continue;
            for (px = 0; px < n; px++) {
                int32_t i_idx = ((px + 1) * angle) >> 5;
                int32_t i_fact = ((px + 1) * angle) & 31;
                int idx = row_base + (int)i_idx;
                int32_t pred;
                if (i_fact != 0)
                    pred = ((32 - i_fact) * ref_arr[idx] + i_fact * ref_arr[idx + 1] + 16) >> 5;
                else
                    pred = ref_arr[idx];
                if (pred < 0) pred = 0;
                if (pred > max_val) pred = max_val;
                dst[px] = (uint16_t)pred;
            }
        }
        if (mode == 10 && c_idx == 0 && size < 32) {
            for (px = 0; px < n; px++) {
                int32_t pred = border[center - 1]
                               + ((border[center + 1 + px] - border[center]) >> 1);
                if (pred < 0) pred = 0;
                if (pred > max_val) pred = max_val;
                plane_put(plane, stride, (int)x + px, (int)y, (uint16_t)pred);
            }
        }
    }
}

int heic_predict_intra(heic_frame *frame, uint32_t x, uint32_t y,
                       uint8_t log2_size, uint8_t mode, uint8_t c_idx,
                       int strong_intra_smoothing)
{
    uint32_t size;
    int32_t border[HEIC_BORDER_N];
    int center = 2 * HEIC_MAX_INTRA;
    uint16_t *plane;
    int stride, plane_n, max_val;

    if (!frame || log2_size > 5) return -1;
    size = 1u << log2_size;
    memset(border, 0, sizeof(border));
    fill_border(frame, x, y, size, c_idx, border, center);

    if (c_idx == 0 || frame->chroma_format == 3)
        sample_filter(border, center, (int)size, c_idx, mode,
                      strong_intra_smoothing, frame->bit_depth);

    if (c_idx == 0) {
        plane = frame->y;
        stride = frame->y_stride;
        plane_n = frame->width * frame->height;
    } else {
        plane = (c_idx == 1) ? frame->cb : frame->cr;
        stride = frame->c_stride;
        plane_n = frame->c_width * frame->c_height;
    }
    if (!plane) return 0;
    max_val = (1 << frame->bit_depth) - 1;
    (void)plane_n;

    if (mode == 0)
        predict_planar(plane, stride, x, y, size, log2_size, max_val, border, center);
    else if (mode == 1)
        predict_dc(plane, stride, x, y, size, log2_size, c_idx, max_val, border, center);
    else
        predict_angular(plane, stride, x, y, size, c_idx, mode, max_val, border, center);
    return 0;
}
