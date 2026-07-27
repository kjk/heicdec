/* hevc_sao.c -- Sample Adaptive Offset filter (H.265 8.7.3)
 *
 * Port of imazen/heic sao.rs. Applied after CTU reconstruction (deblock later).
 */
#include "heic_internal.h"
#include <string.h>

/* EO class → neighbor deltas (dx0, dy0, dx1, dy1) */
static const int EO_OFFSETS[4][4] = {
    { -1, 0, 1, 0 },  /* class 0: horizontal */
    { 0, -1, 0, 1 },  /* class 1: vertical */
    { -1, -1, 1, 1 }, /* class 2: 135° diagonal */
    { 1, -1, -1, 1 }, /* class 3: 45° diagonal */
};

static int isignum(int v)
{
    if (v > 0) return 1;
    if (v < 0) return -1;
    return 0;
}

static int heic_sao_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void apply_sao_band(uint16_t *plane, int stride, int x0, int y0, int x1,
                           int y1, uint8_t band_pos, const int16_t offs[4],
                           int bit_depth)
{
    int max_val = (1 << bit_depth) - 1;
    int band_shift = bit_depth > 5 ? bit_depth - 5 : 0;
    int16_t band_table[32];
    int k, y, x;

    memset(band_table, 0, sizeof(band_table));
    for (k = 0; k < 4; k++)
        band_table[(band_pos + k) & 31] = offs[k];

    if (stride <= 0) return;
    for (y = y0; y < y1; y++) {
        uint16_t *row = plane + (size_t)y * (size_t)stride;
        if (heic_simd_sao_band_row(row, x0, x1, band_shift, band_table, max_val))
            continue;
        for (x = x0; x < x1; x++) {
            int sample = (int)row[x];
            if (sample > max_val) sample = max_val;
            {
                int band = sample >> band_shift;
                int offset = (int)band_table[band & 31];
                if (offset)
                    row[x] = (uint16_t)heic_sao_clampi(sample + offset, 0, max_val);
            }
        }
    }
}

static void apply_sao_edge_pixel(const uint16_t *src, uint16_t *dst, int stride,
                                 int x, int y, int dx0, int dy0, int dx1, int dy1,
                                 int plane_w, int plane_h, int max_val,
                                 const int offset_table[5])
{
    int nx0 = x + dx0, ny0 = y + dy0, nx1 = x + dx1, ny1 = y + dy1;
    int sample, n0, n1, edge_idx, offset;
    size_t idx;

    if (nx0 < 0 || nx0 >= plane_w || ny0 < 0 || ny0 >= plane_h || nx1 < 0 ||
        nx1 >= plane_w || ny1 < 0 || ny1 >= plane_h)
        return;

    idx = (size_t)y * (size_t)stride + (size_t)x;
    sample = (int)src[idx];
    n0 = (int)src[(size_t)ny0 * (size_t)stride + (size_t)nx0];
    n1 = (int)src[(size_t)ny1 * (size_t)stride + (size_t)nx1];
    edge_idx = 2 + isignum(sample - n0) + isignum(sample - n1);
    if (edge_idx < 0 || edge_idx > 4) return;
    offset = offset_table[edge_idx];
    if (offset)
        dst[idx] = (uint16_t)heic_sao_clampi(sample + offset, 0, max_val);
}

static void apply_sao_edge(const uint16_t *src, uint16_t *dst, int stride,
                           int plane_w, int plane_h, int x0, int y0, int x1,
                           int y1, uint8_t eo_class, const int16_t offs[4],
                           int bit_depth)
{
    int max_val = (1 << bit_depth) - 1;
    int dx0, dy0, dx1, dy1;
    int offset_table[5];
    int safe_x0, safe_x1, safe_y0, safe_y1;
    int y, x;
    int cls = (int)(eo_class & 3);

    dx0 = EO_OFFSETS[cls][0];
    dy0 = EO_OFFSETS[cls][1];
    dx1 = EO_OFFSETS[cls][2];
    dy1 = EO_OFFSETS[cls][3];

    /* EO: abs offsets; cats 3/4 apply as negative (spec Table 8-8) */
    offset_table[0] = (int)offs[0];
    offset_table[1] = (int)offs[1];
    offset_table[2] = 0;
    offset_table[3] = -(int)offs[2];
    offset_table[4] = -(int)offs[3];

    if (stride <= 0 || plane_w <= 0 || plane_h <= 0) return;
    if (plane_w > stride) plane_w = stride;

    safe_x0 = x0;
    if (-dx0 > safe_x0) safe_x0 = -dx0;
    if (-dx1 > safe_x0) safe_x0 = -dx1;
    if (safe_x0 < 0) safe_x0 = 0;

    safe_x1 = x1;
    {
        int mx = dx0 > dx1 ? dx0 : dx1;
        if (mx < 0) mx = 0;
        if (plane_w - mx < safe_x1) safe_x1 = plane_w - mx;
    }

    safe_y0 = y0;
    if (-dy0 > safe_y0) safe_y0 = -dy0;
    if (-dy1 > safe_y0) safe_y0 = -dy1;
    if (safe_y0 < 0) safe_y0 = 0;

    safe_y1 = y1;
    {
        int my = dy0 > dy1 ? dy0 : dy1;
        if (my < 0) my = 0;
        if (plane_h - my < safe_y1) safe_y1 = plane_h - my;
    }

    /* Interior: no bounds checks (SIMD for pure H/V classes). */
    for (y = safe_y0; y < safe_y1; y++) {
        const uint16_t *srow = src + (size_t)y * (size_t)stride;
        uint16_t *drow = dst + (size_t)y * (size_t)stride;
        if (cls == 0 &&
            heic_simd_sao_edge_h_row(srow, drow, safe_x0, safe_x1, offset_table, max_val))
            continue;
        if (cls == 1 &&
            heic_simd_sao_edge_v_row(src, dst, stride, y, safe_x0, safe_x1, offset_table,
                                     max_val))
            continue;
        for (x = safe_x0; x < safe_x1; x++) {
            int sample = (int)srow[x];
            int n0 = (int)src[(size_t)(y + dy0) * (size_t)stride + (size_t)(x + dx0)];
            int n1 = (int)src[(size_t)(y + dy1) * (size_t)stride + (size_t)(x + dx1)];
            int edge_idx = 2 + isignum(sample - n0) + isignum(sample - n1);
            int offset = offset_table[edge_idx];
            if (offset)
                drow[x] = (uint16_t)heic_sao_clampi(sample + offset, 0, max_val);
        }
    }

    /* Border: with checks */
    for (y = y0; y < y1; y++) {
        if (y >= safe_y0 && y < safe_y1) {
            for (x = x0; x < safe_x0 && x < x1; x++)
                apply_sao_edge_pixel(src, dst, stride, x, y, dx0, dy0, dx1, dy1,
                                     plane_w, plane_h, max_val, offset_table);
            for (x = safe_x1 > x0 ? safe_x1 : x0; x < x1; x++)
                apply_sao_edge_pixel(src, dst, stride, x, y, dx0, dy0, dx1, dy1,
                                     plane_w, plane_h, max_val, offset_table);
        } else {
            for (x = x0; x < x1; x++)
                apply_sao_edge_pixel(src, dst, stride, x, y, dx0, dy0, dx1, dy1,
                                     plane_w, plane_h, max_val, offset_table);
        }
    }
}

void heic_apply_sao(heic_ctx *ctx, heic_frame *frame, const heic_sao_info *map,
                    uint32_t width_ctbs, uint32_t height_ctbs, uint32_t ctb_size,
                    const uint8_t *pcm_map, uint32_t pcm_stride)
{
    uint32_t ctb_x, ctb_y;
    int need_y = 0, need_cb = 0, need_cr = 0;
    uint16_t *orig_y = NULL, *orig_cb = NULL, *orig_cr = NULL;
    size_t y_n, c_n;
    int w, h, cw, ch, sub_x, sub_y;
    uint32_t n_ctb;

    if (!ctx || !frame || !map || ctb_size == 0) return;
    w = frame->width;
    h = frame->height;
    if (w <= 0 || h <= 0) return;

    n_ctb = width_ctbs * height_ctbs;
    {
        uint32_t i;
        for (i = 0; i < n_ctb; i++) {
            if (map[i].sao_type_idx[0] == 2 ||
                (pcm_map && map[i].sao_type_idx[0] != 0))
                need_y = 1;
            if (map[i].sao_type_idx[1] == 2 ||
                (pcm_map && map[i].sao_type_idx[1] != 0))
                need_cb = 1;
            if (map[i].sao_type_idx[2] == 2 ||
                (pcm_map && map[i].sao_type_idx[2] != 0))
                need_cr = 1;
        }
    }

    y_n = (size_t)frame->y_stride * (size_t)h * sizeof(uint16_t);
    if (need_y && frame->y) {
        orig_y = (uint16_t *)heic_zalloc(ctx, y_n);
        if (orig_y) memcpy(orig_y, frame->y, y_n);
        else need_y = 0;
    }
    cw = frame->c_width;
    ch = frame->c_height;
    c_n = (cw > 0 && ch > 0)
              ? (size_t)frame->c_stride * (size_t)ch * sizeof(uint16_t)
              : 0;
    if (need_cb && frame->cb && c_n) {
        orig_cb = (uint16_t *)heic_zalloc(ctx, c_n);
        if (orig_cb) memcpy(orig_cb, frame->cb, c_n);
        else need_cb = 0;
    }
    if (need_cr && frame->cr && c_n) {
        orig_cr = (uint16_t *)heic_zalloc(ctx, c_n);
        if (orig_cr) memcpy(orig_cr, frame->cr, c_n);
        else need_cr = 0;
    }

    switch (frame->chroma_format) {
    case 1: sub_x = 2; sub_y = 2; break;
    case 2: sub_x = 2; sub_y = 1; break;
    case 3: sub_x = 1; sub_y = 1; break;
    default: sub_x = 1; sub_y = 1; break;
    }

    for (ctb_y = 0; ctb_y < height_ctbs; ctb_y++) {
        for (ctb_x = 0; ctb_x < width_ctbs; ctb_x++) {
            const heic_sao_info *sao = &map[ctb_y * width_ctbs + ctb_x];
            int x_px = (int)(ctb_x * ctb_size);
            int y_px = (int)(ctb_y * ctb_size);
            int x_end = x_px + (int)ctb_size;
            int y_end = y_px + (int)ctb_size;
            if (x_end > w) x_end = w;
            if (y_end > h) y_end = h;

            if (sao->sao_type_idx[0] == 1 && frame->y &&
                (!pcm_map || orig_y)) {
                apply_sao_band(frame->y, frame->y_stride, x_px, y_px, x_end, y_end,
                               sao->sao_band_position[0], sao->sao_offset_val[0],
                               frame->bit_depth);
            } else if (sao->sao_type_idx[0] == 2 && frame->y && orig_y) {
                apply_sao_edge(orig_y, frame->y, frame->y_stride, w, h, x_px, y_px,
                               x_end, y_end, sao->sao_eo_class[0],
                               sao->sao_offset_val[0], frame->bit_depth);
            }

            if (frame->chroma_format > 0 && frame->cb && frame->cr && cw > 0 &&
                ch > 0) {
                int cx0 = x_px / sub_x;
                int cy0 = y_px / sub_y;
                int cx1 = (x_px + (int)ctb_size) / sub_x;
                int cy1 = (y_px + (int)ctb_size) / sub_y;
                if (cx1 > cw) cx1 = cw;
                if (cy1 > ch) cy1 = ch;

                if (sao->sao_type_idx[1] == 1 &&
                    (!pcm_map || orig_cb)) {
                    apply_sao_band(frame->cb, frame->c_stride, cx0, cy0, cx1, cy1,
                                   sao->sao_band_position[1], sao->sao_offset_val[1],
                                   frame->chroma_bit_depth);
                } else if (sao->sao_type_idx[1] == 2 && orig_cb) {
                    apply_sao_edge(orig_cb, frame->cb, frame->c_stride, cw, ch, cx0,
                                   cy0, cx1, cy1, sao->sao_eo_class[1],
                                   sao->sao_offset_val[1], frame->chroma_bit_depth);
                }
                if (sao->sao_type_idx[2] == 1 &&
                    (!pcm_map || orig_cr)) {
                    apply_sao_band(frame->cr, frame->c_stride, cx0, cy0, cx1, cy1,
                                   sao->sao_band_position[2], sao->sao_offset_val[2],
                                   frame->chroma_bit_depth);
                } else if (sao->sao_type_idx[2] == 2 && orig_cr) {
                    apply_sao_edge(orig_cr, frame->cr, frame->c_stride, cw, ch, cx0,
                                   cy0, cx1, cy1, sao->sao_eo_class[2],
                                   sao->sao_offset_val[2], frame->chroma_bit_depth);
                }
            }
        }
    }

    if (pcm_map && pcm_stride > 0) {
        int y, x;
        if (orig_y && frame->y) {
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    if (pcm_map[(size_t)(y / 4) * pcm_stride + x / 4])
                        frame->y[(size_t)y * frame->y_stride + x] =
                            orig_y[(size_t)y * frame->y_stride + x];
                }
            }
        }
        if (frame->chroma_format > 0) {
            for (y = 0; y < ch; y++) {
                for (x = 0; x < cw; x++) {
                    size_t ci = (size_t)y * frame->c_stride + x;
                    size_t pi =
                        (size_t)((y * sub_y) / 4) * pcm_stride +
                        (x * sub_x) / 4;
                    if (!pcm_map[pi]) continue;
                    if (orig_cb && frame->cb) frame->cb[ci] = orig_cb[ci];
                    if (orig_cr && frame->cr) frame->cr[ci] = orig_cr[ci];
                }
            }
        }
    }

    heic_free_buf(ctx, orig_y);
    heic_free_buf(ctx, orig_cb);
    heic_free_buf(ctx, orig_cr);
}
