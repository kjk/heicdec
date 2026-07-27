/* decode.c -- primary image decode orchestration (grid, iden, transforms) */
#include "heic_internal.h"

int heic_hevc_decode(heic_ctx *ctx, const heic_hvcc *cfg,
                     const uint8_t *data, size_t len,
                     heic_frame *out, const heic_abort *ab);
int heic_hevc_decode_ref(heic_ctx *ctx, const heic_hvcc *cfg,
                         const uint8_t *data, size_t len,
                         const heic_frame *ref, heic_frame *out,
                         const heic_abort *ab);
int heic_hevc_decode_refs(heic_ctx *ctx, const heic_hvcc *cfg,
                          const uint8_t *data, size_t len,
                          const heic_frame *const *refs, int n_refs,
                          heic_frame *out, const heic_abort *ab);
int heic_av1_decode(heic_ctx *ctx, const heic_av1c *cfg,
                    const uint8_t *data, size_t len,
                    heic_frame *out, const heic_abort *ab);

static int decode_item(heic_doc *doc, const heic_item *item, heic_frame *frame,
                       const heic_abort *ab, int depth);

/* Decode an hvc1 prediction reference in coded-picture space. Item display
   transforms are deliberately deferred: HEVC motion vectors address the
   reconstructed reference picture, not its rotated/cropped presentation. */
static int decode_hevc_reference(heic_doc *doc, const heic_item *item,
                                 heic_frame *frame, const heic_abort *ab,
                                 int depth)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    int owned = 0;
    uint32_t pred_ids[HEIC_MAX_REF_PICS + 1];
    int n_pred;
    int rc = -1;

    if (depth > 4 || !item->hvcc) {
        heic_error(doc->ctx, HEIC_SEVERITY_ERROR,
                   "invalid HEVC predictive reference chain");
        return -1;
    }
    if (heic_container_item_data(&doc->container, item->id, &data, &len,
                                 &owned) != 0)
        return -1;
    n_pred = heic_container_find_refs(&doc->container, item->id,
                                      HEIC_REF_PRED, pred_ids,
                                      HEIC_MAX_REF_PICS + 1);
    if (n_pred > 0) {
        heic_frame refs[HEIC_MAX_REF_PICS];
        const heic_frame *ref_ptrs[HEIC_MAX_REF_PICS];
        int i;
        memset(refs, 0, sizeof(refs));
        if (n_pred > HEIC_MAX_REF_PICS)
            goto done;
        for (i = 0; i < n_pred; i++) {
            heic_item parent;
            if (heic_container_get_item(&doc->container, pred_ids[i],
                                        &parent) != 0
                || decode_hevc_reference(doc, &parent, &refs[i], ab,
                                         depth + 1) != 0)
                break;
            ref_ptrs[i] = &refs[i];
        }
        if (i == n_pred)
            rc = heic_hevc_decode_refs(doc->ctx, item->hvcc, data, len,
                                       ref_ptrs, n_pred, frame, ab);
        while (i-- > 0) heic_frame_free(doc->ctx, &refs[i]);
    } else {
        rc = heic_hevc_decode(doc->ctx, item->hvcc, data, len, frame, ab);
    }
done:
    if (owned) heic_free_buf(doc->ctx, (void *)data);
    return rc;
}

/* ---- spatial transforms on heic_frame ---- */

static int frame_cropped_w(const heic_frame *f)
{
    return f->width - f->crop_left - f->crop_right;
}
static int frame_cropped_h(const heic_frame *f)
{
    return f->height - f->crop_top - f->crop_bottom;
}

/* Round half away from zero (matches imazen/heic round_f64 for clap). */
static int32_t heic_round_i32(double v)
{
    if (v >= 0.0) return (int32_t)(v + 0.5);
    return (int32_t)(v - 0.5);
}

/* HEIF clean aperture (clap): extra crop on top of SPS conf window. */
static void frame_apply_clap(heic_frame *f, const heic_clap *clap)
{
    int conf_w, conf_h;
    uint32_t clean_w, clean_h, max_extra_h, max_extra_v, extra_left, extra_top;
    uint32_t extra_right, extra_bottom;
    double horiz_off, vert_off;

    if (!f || !clap || !clap->width_d || !clap->height_d) return;
    conf_w = frame_cropped_w(f);
    conf_h = frame_cropped_h(f);
    if (conf_w <= 0 || conf_h <= 0) return;

    clean_w = clap->width_n / clap->width_d;
    clean_h = clap->height_n / clap->height_d;
    if (clean_w == 0 || clean_h == 0) return;
    if (clean_w >= (uint32_t)conf_w && clean_h >= (uint32_t)conf_h) return;
    if (clean_w > (uint32_t)conf_w) clean_w = (uint32_t)conf_w;
    if (clean_h > (uint32_t)conf_h) clean_h = (uint32_t)conf_h;

    horiz_off = clap->horiz_off_d > 0
                    ? (double)clap->horiz_off_n / (double)clap->horiz_off_d
                    : 0.0;
    vert_off = clap->vert_off_d > 0
                   ? (double)clap->vert_off_n / (double)clap->vert_off_d
                   : 0.0;

    max_extra_h = (uint32_t)conf_w - clean_w;
    max_extra_v = (uint32_t)conf_h - clean_h;
    {
        int32_t el = heic_round_i32(((double)conf_w - (double)clean_w) / 2.0 + horiz_off);
        int32_t et = heic_round_i32(((double)conf_h - (double)clean_h) / 2.0 + vert_off);
        if (el < 0) el = 0;
        if (et < 0) et = 0;
        extra_left = (uint32_t)el;
        extra_top = (uint32_t)et;
    }
    if (extra_left > max_extra_h) extra_left = max_extra_h;
    if (extra_top > max_extra_v) extra_top = max_extra_v;
    extra_right = max_extra_h - extra_left;
    extra_bottom = max_extra_v - extra_top;

    f->crop_left += (int)extra_left;
    f->crop_right += (int)extra_right;
    f->crop_top += (int)extra_top;
    f->crop_bottom += (int)extra_bottom;
}

static int frame_materialize_crop(heic_ctx *ctx, heic_frame *f)
{
    heic_frame g;
    int w, h, sub_w = 1, sub_h = 1;
    int y;

    if (!f || (!f->crop_left && !f->crop_right &&
               !f->crop_top && !f->crop_bottom))
        return 0;
    if (f->a)
        return 0;

    w = frame_cropped_w(f);
    h = frame_cropped_h(f);
    if (w <= 0 || h <= 0)
        return 0;
    if (f->chroma_format == 1) {
        sub_w = 2;
        sub_h = 2;
    } else if (f->chroma_format == 2) {
        sub_w = 2;
    }
    /* Moving the origin must not change the chroma sample phase. */
    if ((f->crop_left % sub_w) != 0 || (f->crop_top % sub_h) != 0)
        return 0;

    if (heic_frame_alloc(ctx, &g, w, h, f->bit_depth, f->chroma_format) != 0)
        return -1;
    g.full_range = f->full_range;
    g.matrix_coeffs = f->matrix_coeffs;
    g.color_primaries = f->color_primaries;
    g.transfer_characteristics = f->transfer_characteristics;

    for (y = 0; y < h; y++) {
        const uint16_t *src = f->y +
            (size_t)(f->crop_top + y) * (size_t)f->y_stride +
            (size_t)f->crop_left;
        memcpy(g.y + (size_t)y * (size_t)g.y_stride,
               src, (size_t)w * sizeof(uint16_t));
    }
    if (f->cb && f->cr && g.c_width > 0) {
        int src_x = f->crop_left / sub_w;
        int src_y = f->crop_top / sub_h;
        for (y = 0; y < g.c_height; y++) {
            size_t si = (size_t)(src_y + y) * (size_t)f->c_stride +
                        (size_t)src_x;
            size_t di = (size_t)y * (size_t)g.c_stride;
            memcpy(g.cb + di, f->cb + si,
                   (size_t)g.c_width * sizeof(uint16_t));
            memcpy(g.cr + di, f->cr + si,
                   (size_t)g.c_width * sizeof(uint16_t));
        }
    }

    heic_frame_free(ctx, f);
    *f = g;
    return 0;
}

static int frame_mirror_lr(heic_ctx *ctx, heic_frame *f)
{
    /* Left-right flip (about vertical axis). */
    int y, x, w = f->width, h = f->height;
    (void)ctx;
    for (y = 0; y < h; y++) {
        uint16_t *row = f->y + (size_t)y * (size_t)f->y_stride;
        for (x = 0; x < w / 2; x++) {
            uint16_t t = row[x];
            row[x] = row[w - 1 - x];
            row[w - 1 - x] = t;
        }
    }
    if (f->cb && f->cr && f->c_width > 0) {
        int cw = f->c_width, ch = f->c_height;
        for (y = 0; y < ch; y++) {
            uint16_t *cb = f->cb + (size_t)y * (size_t)f->c_stride;
            uint16_t *cr = f->cr + (size_t)y * (size_t)f->c_stride;
            for (x = 0; x < cw / 2; x++) {
                uint16_t t = cb[x];
                cb[x] = cb[cw - 1 - x];
                cb[cw - 1 - x] = t;
                t = cr[x];
                cr[x] = cr[cw - 1 - x];
                cr[cw - 1 - x] = t;
            }
        }
    }
    {
        int tmp = f->crop_left;
        f->crop_left = f->crop_right;
        f->crop_right = tmp;
    }
    return 0;
}

static int frame_mirror_tb(heic_ctx *ctx, heic_frame *f)
{
    /* Top-bottom flip. */
    int y, x, w = f->width, h = f->height;
    (void)ctx;
    for (y = 0; y < h / 2; y++) {
        uint16_t *a = f->y + (size_t)y * (size_t)f->y_stride;
        uint16_t *b = f->y + (size_t)(h - 1 - y) * (size_t)f->y_stride;
        for (x = 0; x < w; x++) {
            uint16_t t = a[x];
            a[x] = b[x];
            b[x] = t;
        }
    }
    if (f->cb && f->cr && f->c_height > 0) {
        int cw = f->c_width, ch = f->c_height;
        for (y = 0; y < ch / 2; y++) {
            uint16_t *a = f->cb + (size_t)y * (size_t)f->c_stride;
            uint16_t *b = f->cb + (size_t)(ch - 1 - y) * (size_t)f->c_stride;
            uint16_t *c = f->cr + (size_t)y * (size_t)f->c_stride;
            uint16_t *d = f->cr + (size_t)(ch - 1 - y) * (size_t)f->c_stride;
            for (x = 0; x < cw; x++) {
                uint16_t t = a[x];
                a[x] = b[x];
                b[x] = t;
                t = c[x];
                c[x] = d[x];
                d[x] = t;
            }
        }
    }
    {
        int tmp = f->crop_top;
        f->crop_top = f->crop_bottom;
        f->crop_bottom = tmp;
    }
    return 0;
}

static int frame_rotate_90_cw(heic_ctx *ctx, heic_frame *f)
{
    heic_frame g;
    int ow = f->width, oh = f->height;
    int nw = oh, nh = ow;
    int dy, dx;
    if (heic_frame_alloc(ctx, &g, nw, nh, f->bit_depth, f->chroma_format) != 0)
        return -1;
    g.full_range = f->full_range;
    g.matrix_coeffs = f->matrix_coeffs;
    g.color_primaries = f->color_primaries;
    g.transfer_characteristics = f->transfer_characteristics;
    /* dst(dx,dy) = src(dy, oh-1-dx) */
    for (dy = 0; dy < nh; dy++) {
        for (dx = 0; dx < nw; dx++) {
            int sx = dy, sy = oh - 1 - dx;
            g.y[(size_t)dy * (size_t)g.y_stride + (size_t)dx] =
                f->y[(size_t)sy * (size_t)f->y_stride + (size_t)sx];
        }
    }
    if (f->cb && f->cr && f->c_width > 0) {
        int ocw = f->c_width, och = f->c_height;
        int ncw = och, nch = ocw;
        for (dy = 0; dy < nch; dy++) {
            for (dx = 0; dx < ncw; dx++) {
                int sx = dy, sy = och - 1 - dx;
                size_t si = (size_t)sy * (size_t)f->c_stride + (size_t)sx;
                size_t di = (size_t)dy * (size_t)g.c_stride + (size_t)dx;
                g.cb[di] = f->cb[si];
                g.cr[di] = f->cr[si];
            }
        }
    }
    /* crop 90 CW (imazen/libheif): L←B, R←T, T←L, B←R */
    g.crop_left = f->crop_bottom;
    g.crop_right = f->crop_top;
    g.crop_top = f->crop_left;
    g.crop_bottom = f->crop_right;
    heic_frame_free(ctx, f);
    *f = g;
    return 0;
}

static int frame_rotate_180(heic_ctx *ctx, heic_frame *f)
{
    if (frame_mirror_lr(ctx, f) != 0) return -1;
    return frame_mirror_tb(ctx, f);
}

static int frame_rotate_270_cw(heic_ctx *ctx, heic_frame *f)
{
    /* Direct 270 CW (= 90 CCW): dst(dx,dy) = src(ow-1-dy, dx). */
    heic_frame g;
    int ow = f->width, oh = f->height;
    int nw = oh, nh = ow;
    int dy, dx;
    if (heic_frame_alloc(ctx, &g, nw, nh, f->bit_depth, f->chroma_format) != 0)
        return -1;
    g.full_range = f->full_range;
    g.matrix_coeffs = f->matrix_coeffs;
    g.color_primaries = f->color_primaries;
    g.transfer_characteristics = f->transfer_characteristics;
    for (dy = 0; dy < nh; dy++) {
        for (dx = 0; dx < nw; dx++) {
            int sx = ow - 1 - dy, sy = dx;
            g.y[(size_t)dy * (size_t)g.y_stride + (size_t)dx] =
                f->y[(size_t)sy * (size_t)f->y_stride + (size_t)sx];
        }
    }
    if (f->cb && f->cr && f->c_width > 0) {
        int ocw = f->c_width, och = f->c_height;
        int ncw = och, nch = ocw;
        for (dy = 0; dy < nch; dy++) {
            for (dx = 0; dx < ncw; dx++) {
                int sx = ocw - 1 - dy, sy = dx;
                size_t si = (size_t)sy * (size_t)f->c_stride + (size_t)sx;
                size_t di = (size_t)dy * (size_t)g.c_stride + (size_t)dx;
                g.cb[di] = f->cb[si];
                g.cr[di] = f->cr[si];
            }
        }
    }
    /* crop 270 CW: L←T, R←B, T←R, B←L */
    g.crop_left = f->crop_top;
    g.crop_right = f->crop_bottom;
    g.crop_top = f->crop_right;
    g.crop_bottom = f->crop_left;
    heic_frame_free(ctx, f);
    *f = g;
    return 0;
}

static int apply_transforms(heic_ctx *ctx, heic_frame *f, const heic_item *item)
{
    int i;
    for (i = 0; i < item->n_transforms; i++) {
        const heic_xform *t = &item->transforms[i];
        if (t->kind == HEIC_XFORM_IMIR) {
            /* HEIF imir / libheif: axis 0 = vertical flip (T-B), 1 = horizontal (L-R). */
            if (t->imir.axis == 0) {
                if (frame_mirror_tb(ctx, f) != 0) return -1;
            } else {
                if (frame_mirror_lr(ctx, f) != 0) return -1;
            }
        } else if (t->kind == HEIC_XFORM_IROT) {
            if (t->irot.angle != 0 && frame_materialize_crop(ctx, f) != 0)
                return -1;
            switch (t->irot.angle) {
            case 90:
                if (frame_rotate_90_cw(ctx, f) != 0) return -1;
                break;
            case 180:
                if (frame_rotate_180(ctx, f) != 0) return -1;
                break;
            case 270:
                if (frame_rotate_270_cw(ctx, f) != 0) return -1;
                break;
            default:
                break;
            }
        } else if (t->kind == HEIC_XFORM_CLAP) {
            frame_apply_clap(f, &t->clap);
        }
    }
    return 0;
}

/* ---- blit tile into grid canvas ---- */

static void blit_tile(heic_frame *out, const heic_frame *tile, int tile_idx,
                      uint32_t cols, uint32_t tile_w, uint32_t tile_h,
                      uint32_t out_w, uint32_t out_h)
{
    uint32_t tile_row = (uint32_t)tile_idx / cols;
    uint32_t tile_col = (uint32_t)tile_idx % cols;
    uint32_t dst_x = tile_col * tile_w;
    uint32_t dst_y = tile_row * tile_h;
    uint32_t copy_w, copy_h, row, col;
    uint32_t src_x0 = (uint32_t)tile->crop_left;
    uint32_t src_y0 = (uint32_t)tile->crop_top;
    uint32_t tw = (uint32_t)frame_cropped_w(tile);
    uint32_t th = (uint32_t)frame_cropped_h(tile);

    if (dst_x >= out_w || dst_y >= out_h) return;
    copy_w = tw;
    if (copy_w > out_w - dst_x) copy_w = out_w - dst_x;
    copy_h = th;
    if (copy_h > out_h - dst_y) copy_h = out_h - dst_y;

    for (row = 0; row < copy_h; row++) {
        const uint16_t *src =
            tile->y + (size_t)(src_y0 + row) * (size_t)tile->y_stride + src_x0;
        uint16_t *dst =
            out->y + (size_t)(dst_y + row) * (size_t)out->y_stride + dst_x;
        memcpy(dst, src, (size_t)copy_w * sizeof(uint16_t));
    }

    if (out->chroma_format > 0 && tile->cb && out->cb) {
        uint32_t sub_x = 2, sub_y = 2;
        uint32_t c_copy_w, c_copy_h, c_dst_x, c_dst_y, c_src_x, c_src_y;
        if (out->chroma_format == 2) {
            sub_x = 2;
            sub_y = 1;
        } else if (out->chroma_format == 3) {
            sub_x = 1;
            sub_y = 1;
        }
        c_copy_w = (copy_w + sub_x - 1) / sub_x;
        c_copy_h = (copy_h + sub_y - 1) / sub_y;
        c_dst_x = dst_x / sub_x;
        c_dst_y = dst_y / sub_y;
        c_src_x = src_x0 / sub_x;
        c_src_y = src_y0 / sub_y;
        for (row = 0; row < c_copy_h; row++) {
            if (c_dst_y + row >= (uint32_t)out->c_height) break;
            if (c_src_y + row >= (uint32_t)tile->c_height) break;
            for (col = 0; col < c_copy_w; col++) {
                size_t si, di;
                if (c_dst_x + col >= (uint32_t)out->c_width) break;
                if (c_src_x + col >= (uint32_t)tile->c_width) break;
                si = (size_t)(c_src_y + row) * (size_t)tile->c_stride + (c_src_x + col);
                di = (size_t)(c_dst_y + row) * (size_t)out->c_stride + (c_dst_x + col);
                out->cb[di] = tile->cb[si];
                out->cr[di] = tile->cr[si];
            }
        }
    }
}

/* ---- grid ---- */

static int decode_grid(heic_doc *doc, const heic_item *grid_item, heic_frame *out,
                       const heic_abort *ab, int depth)
{
    const uint8_t *gdata = NULL;
    size_t glen = 0;
    int owned = 0;
    uint8_t flags;
    uint32_t rows, cols, out_w, out_h;
    uint32_t tile_ids[512];
    int n_tiles, expected, ti;
    heic_item first;
    uint32_t tile_w, tile_h;
    int bit_depth, chroma;

    if (heic_container_item_data(&doc->container, grid_item->id, &gdata, &glen, &owned) != 0)
        return -1;
    if (glen < 8) {
        if (owned) heic_free_buf(doc->ctx, (void *)gdata);
        return -1;
    }
    flags = gdata[1];
    rows = (uint32_t)gdata[2] + 1;
    cols = (uint32_t)gdata[3] + 1;
    if (flags & 1) {
        if (glen < 12) {
            if (owned) heic_free_buf(doc->ctx, (void *)gdata);
            return -1;
        }
        out_w = heic_rb32(gdata + 4);
        out_h = heic_rb32(gdata + 8);
    } else {
        out_w = heic_rb16(gdata + 4);
        out_h = heic_rb16(gdata + 6);
    }
    if (owned) heic_free_buf(doc->ctx, (void *)gdata);

    if (out_w == 0 || out_h == 0 || out_w > doc->ctx->limits.max_width ||
        out_h > doc->ctx->limits.max_height)
        return -1;
    if ((uint64_t)out_w * out_h > doc->ctx->limits.max_pixels) return -1;

    expected = (int)(rows * cols);
    if (expected <= 0 || expected > 512) {
        heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "grid tile count out of range");
        return -1;
    }
    n_tiles = heic_container_find_refs(&doc->container, grid_item->id, HEIC_REF_DIMG,
                                       tile_ids, expected);
    if (n_tiles != expected) {
        heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "grid tile count mismatch %d vs %d",
                   n_tiles, expected);
        return -1;
    }
    if (heic_container_get_item(&doc->container, tile_ids[0], &first) != 0) return -1;
    if (!first.has_dims) return -1;
    /* Layout uses display size (ispe after clap / 90° transforms), not coded size. */
    tile_w = first.width;
    tile_h = first.height;
    {
        int i;
        for (i = 0; i < first.n_transforms; i++) {
            const heic_xform *t = &first.transforms[i];
            if (t->kind == HEIC_XFORM_IROT &&
                (t->irot.angle == 90 || t->irot.angle == 270)) {
                uint32_t tmp = tile_w;
                tile_w = tile_h;
                tile_h = tmp;
            } else if (t->kind == HEIC_XFORM_CLAP && t->clap.width_d &&
                       t->clap.height_d) {
                uint32_t cw = t->clap.width_n / t->clap.width_d;
                uint32_t ch = t->clap.height_n / t->clap.height_d;
                if (cw > 0 && ch > 0) {
                    tile_w = cw;
                    tile_h = ch;
                }
            }
        }
    }

    if (first.hvcc) {
        bit_depth = 8 + first.hvcc->bit_depth_luma_minus8;
        chroma = first.hvcc->chroma_format;
    } else if (first.av1c) {
        bit_depth = first.av1c->high_bitdepth ? (first.av1c->twelve_bit ? 12 : 10) : 8;
        chroma = first.av1c->monochrome
                     ? 0
                     : (first.av1c->chroma_subsampling_x
                            ? (first.av1c->chroma_subsampling_y ? 1 : 2)
                            : 3);
    } else {
        heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "grid tiles missing codec config");
        return -1;
    }

    if (heic_frame_alloc(doc->ctx, out, (int)out_w, (int)out_h, bit_depth, chroma) != 0)
        return -1;

    for (ti = 0; ti < n_tiles; ti++) {
        heic_item tile_item;
        heic_frame tile_frame;
        if (heic_abort_check(ab)) {
            heic_frame_free(doc->ctx, out);
            return -1;
        }
        if (heic_container_get_item(&doc->container, tile_ids[ti], &tile_item) != 0) {
            heic_frame_free(doc->ctx, out);
            return -1;
        }
        memset(&tile_frame, 0, sizeof(tile_frame));
        if (decode_item(doc, &tile_item, &tile_frame, ab, depth + 1) != 0) {
            heic_frame_free(doc->ctx, &tile_frame);
            heic_frame_free(doc->ctx, out);
            heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "grid tile %d decode failed", ti);
            return -1;
        }
        if (ti == 0) {
            out->full_range = tile_frame.full_range;
            out->matrix_coeffs = tile_frame.matrix_coeffs;
            out->color_primaries = tile_frame.color_primaries;
            out->transfer_characteristics = tile_frame.transfer_characteristics;
        }
        blit_tile(out, &tile_frame, ti, cols, tile_w, tile_h, out_w, out_h);
        heic_frame_free(doc->ctx, &tile_frame);
    }
    /* Output crop is identity — grid dims are already display size */
    out->crop_left = out->crop_right = out->crop_top = out->crop_bottom = 0;
    return 0;
}

/* ---- overlay (iovl) ---- */

static void blit_overlay(heic_frame *out, const heic_frame *tile, int32_t off_x,
                         int32_t off_y)
{
    /* Clip negative offsets: draw on-canvas portion starting at (0,0) from
       tile origin advanced by -off. */
    int32_t src_x = 0, src_y = 0;
    int32_t dst_x = off_x, dst_y = off_y;
    int32_t tw = frame_cropped_w(tile);
    int32_t th = frame_cropped_h(tile);
    int32_t copy_w, copy_h, row, col;
    int32_t tile_sx = tile->crop_left, tile_sy = tile->crop_top;

    if (dst_x < 0) {
        src_x = -dst_x;
        dst_x = 0;
    }
    if (dst_y < 0) {
        src_y = -dst_y;
        dst_y = 0;
    }
    if (src_x >= tw || src_y >= th) return;
    if (dst_x >= out->width || dst_y >= out->height) return;

    copy_w = tw - src_x;
    if (copy_w > out->width - dst_x) copy_w = out->width - dst_x;
    copy_h = th - src_y;
    if (copy_h > out->height - dst_y) copy_h = out->height - dst_y;
    if (copy_w <= 0 || copy_h <= 0) return;

    for (row = 0; row < copy_h; row++) {
        const uint16_t *s = tile->y +
            (size_t)(tile_sy + src_y + row) * (size_t)tile->y_stride +
            (size_t)(tile_sx + src_x);
        uint16_t *d =
            out->y + (size_t)(dst_y + row) * (size_t)out->y_stride + (size_t)dst_x;
        memcpy(d, s, (size_t)copy_w * sizeof(uint16_t));
    }
    if (out->cb && tile->cb && out->chroma_format > 0) {
        uint32_t sub_x = 2, sub_y = 2;
        int32_t c_src_x, c_src_y, c_dst_x, c_dst_y, c_w, c_h;
        if (out->chroma_format == 2) {
            sub_x = 2;
            sub_y = 1;
        } else if (out->chroma_format == 3) {
            sub_x = 1;
            sub_y = 1;
        }
        c_src_x = (tile_sx + src_x) / (int32_t)sub_x;
        c_src_y = (tile_sy + src_y) / (int32_t)sub_y;
        c_dst_x = dst_x / (int32_t)sub_x;
        c_dst_y = dst_y / (int32_t)sub_y;
        c_w = (copy_w + (int32_t)sub_x - 1) / (int32_t)sub_x;
        c_h = (copy_h + (int32_t)sub_y - 1) / (int32_t)sub_y;
        for (row = 0; row < c_h; row++) {
            if (c_dst_y + row >= out->c_height || c_src_y + row >= tile->c_height)
                break;
            for (col = 0; col < c_w; col++) {
                size_t si, di;
                if (c_dst_x + col >= out->c_width || c_src_x + col >= tile->c_width)
                    break;
                si = (size_t)(c_src_y + row) * (size_t)tile->c_stride +
                     (size_t)(c_src_x + col);
                di = (size_t)(c_dst_y + row) * (size_t)out->c_stride +
                     (size_t)(c_dst_x + col);
                out->cb[di] = tile->cb[si];
                out->cr[di] = tile->cr[si];
            }
        }
    }
}

static int decode_iovl(heic_doc *doc, const heic_item *iovl_item, heic_frame *out,
                       const heic_abort *ab, int depth)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    int owned = 0;
    uint8_t version, flags;
    int large;
    uint32_t tile_ids[64];
    int n_tiles, i;
    uint16_t fill[4];
    size_t pos;
    uint32_t canvas_w, canvas_h;
    int32_t offsets[64][2];
    heic_item first;
    int bit_depth, chroma;
    uint16_t fill_y, fill_cb, fill_cr;
    size_t n;

    if (heic_container_item_data(&doc->container, iovl_item->id, &data, &len, &owned) != 0)
        return -1;
    if (len < 2 + 8 + 4) {
        if (owned) heic_free_buf(doc->ctx, (void *)data);
        return -1;
    }
    version = data[0];
    flags = data[1];
    if (version != 0) {
        heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "overlay version != 0");
        if (owned) heic_free_buf(doc->ctx, (void *)data);
        return -1;
    }
    large = (flags & 1) != 0;
    n_tiles = heic_container_find_refs(&doc->container, iovl_item->id, HEIC_REF_DIMG,
                                       tile_ids, 64);
    if (n_tiles < 1) {
        if (owned) heic_free_buf(doc->ctx, (void *)data);
        return -1;
    }
    for (i = 0; i < 4; i++) fill[i] = heic_rb16(data + 2 + i * 2);
    pos = 2 + 8;
    if (large) {
        if (pos + 8 > len) {
            if (owned) heic_free_buf(doc->ctx, (void *)data);
            return -1;
        }
        canvas_w = heic_rb32(data + pos);
        canvas_h = heic_rb32(data + pos + 4);
        pos += 8;
    } else {
        if (pos + 4 > len) {
            if (owned) heic_free_buf(doc->ctx, (void *)data);
            return -1;
        }
        canvas_w = heic_rb16(data + pos);
        canvas_h = heic_rb16(data + pos + 2);
        pos += 4;
    }
    for (i = 0; i < n_tiles; i++) {
        if (large) {
            if (pos + 8 > len) break;
            offsets[i][0] = (int32_t)heic_rb32(data + pos);
            offsets[i][1] = (int32_t)heic_rb32(data + pos + 4);
            pos += 8;
        } else {
            if (pos + 4 > len) break;
            offsets[i][0] = (int16_t)heic_rb16(data + pos);
            offsets[i][1] = (int16_t)heic_rb16(data + pos + 2);
            pos += 4;
        }
    }
    if (owned) {
        heic_free_buf(doc->ctx, (void *)data);
        owned = 0;
    }

    if (canvas_w == 0 || canvas_h == 0 || canvas_w > doc->ctx->limits.max_width ||
        canvas_h > doc->ctx->limits.max_height)
        return -1;

    if (heic_container_get_item(&doc->container, tile_ids[0], &first) != 0) return -1;
    if (first.hvcc) {
        bit_depth = 8 + first.hvcc->bit_depth_luma_minus8;
        chroma = first.hvcc->chroma_format;
    } else {
        bit_depth = 8;
        chroma = 1;
    }
    if (heic_frame_alloc(doc->ctx, out, (int)canvas_w, (int)canvas_h, bit_depth, chroma) != 0)
        return -1;

    /* Fill canvas: use high 8 bits of R/G/B as crude Y fill (preview-quality).
       Proper RGB→YCbCr can wait; stills use solid fills. */
    fill_y = (uint16_t)((fill[0] >> 8) << (bit_depth > 8 ? bit_depth - 8 : 0));
    fill_cb = (uint16_t)(128 << (bit_depth > 8 ? bit_depth - 8 : 0));
    fill_cr = fill_cb;
    n = (size_t)out->width * (size_t)out->height;
    for (i = 0; i < (int)n; i++) out->y[i] = fill_y;
    if (out->cb) {
        size_t cn = (size_t)out->c_width * (size_t)out->c_height;
        size_t k;
        for (k = 0; k < cn; k++) {
            out->cb[k] = fill_cb;
            out->cr[k] = fill_cr;
        }
    }

    for (i = 0; i < n_tiles; i++) {
        heic_item tile_item;
        heic_frame tile;
        if (heic_abort_check(ab)) {
            heic_frame_free(doc->ctx, out);
            return -1;
        }
        if (heic_container_get_item(&doc->container, tile_ids[i], &tile_item) != 0) {
            heic_frame_free(doc->ctx, out);
            return -1;
        }
        memset(&tile, 0, sizeof(tile));
        if (decode_item(doc, &tile_item, &tile, ab, depth + 1) != 0) {
            heic_frame_free(doc->ctx, &tile);
            heic_frame_free(doc->ctx, out);
            return -1;
        }
        if (i == 0) {
            out->full_range = tile.full_range;
            out->matrix_coeffs = tile.matrix_coeffs;
        }
        blit_overlay(out, &tile, offsets[i][0], offsets[i][1]);
        heic_frame_free(doc->ctx, &tile);
    }
    out->crop_left = out->crop_right = out->crop_top = out->crop_bottom = 0;
    return 0;
}

/* ---- per-item decode ---- */

static int decode_item(heic_doc *doc, const heic_item *item, heic_frame *frame,
                       const heic_abort *ab, int depth)
{
    const uint8_t *data = NULL;
    size_t len = 0;
    int owned = 0;
    int rc = -1;

    /* Grids/overlays/iden chains: keep shallow to bound stack under ASan. */
    if (depth > 4) {
        heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "derived item recursion too deep");
        return -1;
    }
    if (heic_abort_check(ab)) return -1;

    if (item->item_type == HEIC_TYPE_GRID) {
        rc = decode_grid(doc, item, frame, ab, depth);
        if (rc == 0 && item->colr && item->colr->kind == HEIC_COLR_NCLX) {
            frame->color_primaries = (uint8_t)item->colr->color_primaries;
            frame->transfer_characteristics =
                (uint8_t)item->colr->transfer_characteristics;
            frame->matrix_coeffs = (uint8_t)item->colr->matrix_coefficients;
            frame->full_range = item->colr->full_range;
        }
        if (rc == 0) rc = apply_transforms(doc->ctx, frame, item);
        return rc;
    }

    if (item->item_type == HEIC_TYPE_IDEN || item->item_type == HEIC_TYPE_TMAP) {
        uint32_t refs[8];
        heic_item child;
        int n = heic_container_find_refs(&doc->container, item->id, HEIC_REF_DIMG, refs, 8);
        if (n < 1) {
            heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "iden/tmap missing dimg");
            return -1;
        }
        if (heic_container_get_item(&doc->container, refs[0], &child) != 0) return -1;
        rc = decode_item(doc, &child, frame, ab, depth + 1);
        if (rc == 0) rc = apply_transforms(doc->ctx, frame, item);
        return rc;
    }

    if (heic_container_item_data(&doc->container, item->id, &data, &len, &owned) != 0)
        return -1;

    if (item->item_type == HEIC_TYPE_HVC1 || item->hvcc) {
        uint32_t pred_ids[HEIC_MAX_REF_PICS + 1];
        int n_pred;
        if (!item->hvcc) {
            heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "hvc1 item missing hvcC");
            goto done;
        }
        n_pred = heic_container_find_refs(&doc->container, item->id,
                                          HEIC_REF_PRED, pred_ids,
                                          HEIC_MAX_REF_PICS + 1);
        if (n_pred > 0) {
            heic_frame refs[HEIC_MAX_REF_PICS];
            const heic_frame *ref_ptrs[HEIC_MAX_REF_PICS];
            int i;
            memset(refs, 0, sizeof(refs));
            if (n_pred > HEIC_MAX_REF_PICS) {
                heic_error(doc->ctx, HEIC_SEVERITY_ERROR,
                           "predictive item has too many pred references");
                goto done;
            }
            for (i = 0; i < n_pred; i++) {
                heic_item ref_item;
                if (heic_container_get_item(&doc->container, pred_ids[i],
                                            &ref_item) != 0
                    || decode_hevc_reference(doc, &ref_item, &refs[i], ab,
                                             depth + 1) != 0)
                    break;
                ref_ptrs[i] = &refs[i];
            }
            if (i == n_pred)
                rc = heic_hevc_decode_refs(doc->ctx, item->hvcc, data, len,
                                           ref_ptrs, n_pred, frame, ab);
            while (i-- > 0) heic_frame_free(doc->ctx, &refs[i]);
        } else {
            rc = heic_hevc_decode(doc->ctx, item->hvcc, data, len, frame, ab);
        }
    } else if (item->item_type == HEIC_TYPE_AV01 || item->av1c) {
        if (!item->av1c) {
            heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "av01 item missing av1C");
            goto done;
        }
        rc = heic_av1_decode(doc->ctx, item->av1c, data, len, frame, ab);
    } else if (item->item_type == HEIC_TYPE_UNCI || item->uncc) {
        if (!item->uncc) {
            heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "unci item missing uncC");
            goto done;
        }
        {
            uint32_t w = item->has_dims ? item->width : 0;
            uint32_t h = item->has_dims ? item->height : 0;
            if (!w || !h) {
                heic_error(doc->ctx, HEIC_SEVERITY_ERROR, "unci item missing ispe");
                goto done;
            }
            rc = heic_unci_decode(doc->ctx, item->uncc, item->cmpc, item->cmpd, item->icef,
                                  data, len, w, h, frame, ab);
        }
    } else if (item->item_type == HEIC_TYPE_IOVL) {
        if (owned) {
            heic_free_buf(doc->ctx, (void *)data);
            owned = 0;
        }
        rc = decode_iovl(doc, item, frame, ab, depth);
        if (rc == 0) rc = apply_transforms(doc->ctx, frame, item);
        return rc;
    } else {
        heic_error(doc->ctx, HEIC_SEVERITY_ERROR,
                   "unsupported item type (hvc1/av01/unci/grid/iden/iovl/tmap)");
        rc = -1;
    }

    if (rc == 0 && item->colr && item->colr->kind == HEIC_COLR_NCLX) {
        frame->color_primaries = (uint8_t)item->colr->color_primaries;
        frame->transfer_characteristics = (uint8_t)item->colr->transfer_characteristics;
        frame->matrix_coeffs = (uint8_t)item->colr->matrix_coefficients;
        frame->full_range = item->colr->full_range;
    }
    if (rc == 0) rc = apply_transforms(doc->ctx, frame, item);
done:
    if (owned) heic_free_buf(doc->ctx, (void *)data);
    return rc;
}

/* Scale alpha sample to primary bit depth (shift left/right). */
static uint16_t scale_alpha_sample(uint16_t v, int src_bd, int dst_bd)
{
    if (v == HEIC_UNINIT_SAMPLE) return 0;
    if (src_bd == dst_bd || src_bd <= 0 || dst_bd <= 0) return v;
    if (src_bd < dst_bd) return (uint16_t)(v << (dst_bd - src_bd));
    return (uint16_t)(v >> (src_bd - dst_bd));
}

static int attach_decoded_alpha(heic_ctx *ctx, heic_frame *frame,
                                const heic_frame *alpha)
{
    int pw, ph, aw, ah, y, x;
    int abd, pbd;
    if (frame->a) return 0;
    pw = frame->width - frame->crop_left - frame->crop_right;
    ph = frame->height - frame->crop_top - frame->crop_bottom;
    aw = alpha->width - alpha->crop_left - alpha->crop_right;
    ah = alpha->height - alpha->crop_top - alpha->crop_bottom;
    if (pw <= 0 || ph <= 0 || aw <= 0 || ah <= 0 || !alpha->y)
        return -1;
    frame->a = (uint16_t *)heic_zalloc(
        ctx, (size_t)frame->width * (size_t)frame->height * sizeof(uint16_t));
    if (!frame->a) return -1;
    frame->a_stride = frame->width;
    pbd = frame->bit_depth > 0 ? frame->bit_depth : 8;
    abd = alpha->bit_depth > 0 ? alpha->bit_depth : 8;
    {
        size_t i, n = (size_t)frame->width * (size_t)frame->height;
        uint16_t opaque = (uint16_t)((1u << pbd) - 1);
        for (i = 0; i < n; i++) frame->a[i] = opaque;
    }
    for (y = 0; y < ph; y++) {
        int sy = ah == ph ? y : (int)((int64_t)y * ah / ph);
        if (sy >= ah) sy = ah - 1;
        for (x = 0; x < pw; x++) {
            int sx = aw == pw ? x : (int)((int64_t)x * aw / pw);
            uint16_t v;
            if (sx >= aw) sx = aw - 1;
            v = alpha->y[
                (size_t)(alpha->crop_top + sy) * (size_t)alpha->y_stride
                + (size_t)(alpha->crop_left + sx)];
            frame->a[
                (size_t)(frame->crop_top + y) * (size_t)frame->a_stride
                + (size_t)(frame->crop_left + x)] =
                    scale_alpha_sample(v, abd, pbd);
        }
    }
    return 0;
}

/* Attach auxiliary alpha plane (auxl) sized to primary cropped dims.
 * Alpha item may be hvc1, av01, or a derived grid/iden (full decode_item). */
static void attach_alpha(heic_doc *doc, uint32_t primary_id, heic_frame *frame,
                         const heic_abort *ab)
{
    uint32_t aux_ids[4];
    int n;
    heic_item alpha_item;
    heic_frame alpha;

    /* Prefer CICP alpha URN (AVIF); fall back to HEVC auxid:1. */
    n = heic_container_find_aux(&doc->container, primary_id,
                                "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha",
                                aux_ids, 4);
    if (n <= 0)
        n = heic_container_find_aux(&doc->container, primary_id,
                                    "urn:mpeg:hevc:2015:auxid:1", aux_ids, 4);
    if (n <= 0) return;
    if (heic_container_get_item(&doc->container, aux_ids[0], &alpha_item) != 0) return;
    memset(&alpha, 0, sizeof(alpha));
    if (decode_item(doc, &alpha_item, &alpha, ab, 1) != 0) {
        heic_error(doc->ctx, HEIC_SEVERITY_WARNING, "alpha item decode failed");
        heic_frame_free(doc->ctx, &alpha);
        return;
    }
    (void)attach_decoded_alpha(doc->ctx, frame, &alpha);
    heic_frame_free(doc->ctx, &alpha);
}

/* ---- public orchestration ---- */

int heic_decode_primary(heic_doc *doc, heic_format format,
                        heic_image **out_img, uint8_t *into, size_t into_size,
                        int into_stride, const heic_abort *ab)
{
    heic_item item;
    heic_frame frame;
    heic_image_info info;
    int bpp, need_stride;
    size_t need;
    uint8_t *dst;
    heic_image *img = NULL;
    int rc;
    int out_w, out_h;

    if (!doc) return -1;
    memset(&frame, 0, sizeof(frame));

    if (heic_container_get_item(&doc->container, doc->container.primary_item_id, &item) != 0)
        return -1;
    if (heic_doc_info(doc, &info) != 0) return -1;

    bpp = (format == HEIC_FORMAT_RGBA || format == HEIC_FORMAT_BGRA) ? 4 : 3;
    need_stride = into_stride > 0 ? into_stride : (int)info.width * bpp;
    need = (size_t)need_stride * (size_t)info.height;

    if (heic_abort_check(ab)) return -1;
    rc = decode_item(doc, &item, &frame, ab, 0);
    if (rc != 0) {
        heic_frame_free(doc->ctx, &frame);
        return -1;
    }
    attach_alpha(doc, item.id, &frame, ab);

    out_w = frame_cropped_w(&frame);
    out_h = frame_cropped_h(&frame);
    if (out_w <= 0 || out_h <= 0) {
        heic_frame_free(doc->ctx, &frame);
        return -1;
    }
    /* Prefer actual decoded dims if transforms swapped them */
    need_stride = into_stride > 0 ? into_stride : out_w * bpp;
    need = (size_t)need_stride * (size_t)out_h;

    if (into) {
        if (into_size < need) {
            heic_frame_free(doc->ctx, &frame);
            return -1;
        }
        dst = into;
        if (heic_frame_to_rgb(doc->ctx, &frame, format, dst, need_stride) != 0) {
            heic_frame_free(doc->ctx, &frame);
            return -1;
        }
        heic_frame_free(doc->ctx, &frame);
        return 0;
    }

    img = (heic_image *)heic_zalloc(doc->ctx, sizeof(heic_image));
    if (!img) {
        heic_frame_free(doc->ctx, &frame);
        return -1;
    }
    img->width = (uint32_t)out_w;
    img->height = (uint32_t)out_h;
    img->format = format;
    img->stride = out_w * bpp;
    img->data = (uint8_t *)doc->ctx->alloc(doc->ctx->user, doc->ctx, need);
    if (!img->data) {
        heic_free_buf(doc->ctx, img);
        heic_frame_free(doc->ctx, &frame);
        return -1;
    }
    if (heic_frame_to_rgb(doc->ctx, &frame, format, img->data, img->stride) != 0) {
        heic_image_destroy(doc->ctx, img);
        heic_frame_free(doc->ctx, &frame);
        return -1;
    }
    heic_frame_free(doc->ctx, &frame);
    if (out_img) *out_img = img;
    else heic_image_destroy(doc->ctx, img);
    return 0;
}

heic_image *heic_doc_decode(heic_doc *doc, heic_format format)
{
    heic_image *img = NULL;
    if (heic_decode_primary(doc, format, &img, NULL, 0, 0, NULL) != 0) return NULL;
    return img;
}

heic_image *heic_doc_decode_abortable(heic_doc *doc, heic_format format, heic_abort *ab)
{
    heic_image *img = NULL;
    if (heic_decode_primary(doc, format, &img, NULL, 0, 0, ab) != 0) return NULL;
    return img;
}

static heic_image *sequence_frame_to_image(heic_ctx *ctx, heic_frame *frame,
                                           heic_format format)
{
    heic_image *img;
    int bpp, w = frame_cropped_w(frame), h = frame_cropped_h(frame);
    size_t need;
    if (w <= 0 || h <= 0) return NULL;
    bpp = (format == HEIC_FORMAT_RGBA || format == HEIC_FORMAT_BGRA) ? 4 : 3;
    if ((size_t)w > SIZE_MAX / (size_t)bpp
        || (size_t)w * (size_t)bpp > SIZE_MAX / (size_t)h)
        return NULL;
    need = (size_t)w * (size_t)h * (size_t)bpp;
    img = (heic_image *)heic_zalloc(ctx, sizeof(heic_image));
    if (!img) return NULL;
    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    img->format = format;
    img->stride = w * bpp;
    img->data = (uint8_t *)ctx->alloc(ctx->user, ctx, need);
    if (!img->data
        || heic_frame_to_rgb(ctx, frame, format, img->data, img->stride) != 0) {
        heic_image_destroy(ctx, img);
        return NULL;
    }
    return img;
}

typedef struct {
    heic_frame *pictures;
    uint8_t *picture_ready;
    const heic_frame **refs;
    heic_av1_sequence_state *av1;
    uint32_t sample_count;
    uint32_t cache_start;
    uint32_t next_sample;
    int initialized;
} heic_sequence_cache;

struct heic_sequence_decoder {
    heic_doc *doc;
    heic_format format;
    heic_sequence_cache color;
    heic_sequence_cache alpha;
};

static void sequence_cache_clear(heic_sequence_decoder *decoder,
                                 heic_sequence_cache *cache)
{
    uint32_t i;
    if (!decoder || !cache) return;
    if (cache->pictures && cache->picture_ready) {
        for (i = 0; i < cache->sample_count; i++) {
            if (!cache->picture_ready[i]) continue;
            heic_frame_free(decoder->doc->ctx, &cache->pictures[i]);
            cache->picture_ready[i] = 0;
        }
    }
    cache->cache_start = 0;
    cache->next_sample = 0;
    cache->initialized = 0;
    heic_av1_sequence_destroy(cache->av1);
    cache->av1 = NULL;
}

static int sequence_cache_init(heic_sequence_decoder *decoder,
                               heic_sequence_cache *cache,
                               const heic_sequence *seq)
{
    heic_ctx *ctx = decoder->doc->ctx;
    size_t pictures_size, refs_size;
    if (!seq || !seq->sample_count
        || (size_t)seq->sample_count > SIZE_MAX / sizeof(heic_frame)
        || (size_t)seq->sample_count > SIZE_MAX / sizeof(heic_frame *))
        return -1;
    pictures_size = (size_t)seq->sample_count * sizeof(heic_frame);
    refs_size = (size_t)seq->sample_count * sizeof(heic_frame *);
    cache->sample_count = seq->sample_count;
    cache->pictures = (heic_frame *)heic_zalloc(ctx, pictures_size);
    cache->picture_ready =
        (uint8_t *)heic_zalloc(ctx, seq->sample_count);
    cache->refs =
        (const heic_frame **)heic_zalloc(ctx, refs_size);
    return cache->pictures && cache->picture_ready && cache->refs ? 0 : -1;
}

static void sequence_cache_destroy(heic_sequence_decoder *decoder,
                                   heic_sequence_cache *cache)
{
    heic_ctx *ctx = decoder->doc->ctx;
    sequence_cache_clear(decoder, cache);
    heic_free_buf(ctx, cache->refs);
    heic_free_buf(ctx, cache->picture_ready);
    heic_free_buf(ctx, cache->pictures);
    memset(cache, 0, sizeof(*cache));
}

heic_sequence_decoder *heic_sequence_decoder_new(heic_doc *doc,
                                                 heic_format format)
{
    const heic_sequence *seq;
    heic_sequence_decoder *decoder;
    if (!doc || !(seq = doc->container.sequence) || !seq->sample_count)
        return NULL;
    if (format < HEIC_FORMAT_RGB || format > HEIC_FORMAT_BGRA)
        return NULL;
    decoder = (heic_sequence_decoder *)heic_zalloc(doc->ctx, sizeof(*decoder));
    if (!decoder) return NULL;
    decoder->doc = doc;
    decoder->format = format;
    if (sequence_cache_init(decoder, &decoder->color, seq) != 0
        || (seq->alpha
            && (format == HEIC_FORMAT_RGBA || format == HEIC_FORMAT_BGRA)
            && sequence_cache_init(
                   decoder, &decoder->alpha, seq->alpha) != 0)) {
        heic_sequence_decoder_destroy(decoder);
        return NULL;
    }
    return decoder;
}

void heic_sequence_decoder_reset(heic_sequence_decoder *decoder)
{
    sequence_cache_clear(decoder, &decoder->color);
    sequence_cache_clear(decoder, &decoder->alpha);
}

void heic_sequence_decoder_destroy(heic_sequence_decoder *decoder)
{
    heic_ctx *ctx;
    if (!decoder) return;
    ctx = decoder->doc->ctx;
    sequence_cache_destroy(decoder, &decoder->alpha);
    sequence_cache_destroy(decoder, &decoder->color);
    heic_free_buf(ctx, decoder);
}

static void sequence_apply_color(const heic_item *item, heic_frame *decoded)
{
    if (!item->colr || item->colr->kind != HEIC_COLR_NCLX) return;
    decoded->color_primaries = (uint8_t)item->colr->color_primaries;
    decoded->transfer_characteristics =
        (uint8_t)item->colr->transfer_characteristics;
    decoded->matrix_coeffs = (uint8_t)item->colr->matrix_coefficients;
    decoded->full_range = item->colr->full_range;
}

static int sequence_store_av1_picture(heic_sequence_decoder *decoder,
                                      heic_sequence_cache *cache,
                                      const heic_item *item,
                                      heic_frame *picture,
                                      uint32_t sample)
{
    if (sample < cache->cache_start || sample >= cache->next_sample
        || sample >= cache->sample_count
        || cache->picture_ready[sample]) {
        heic_frame_free(decoder->doc->ctx, picture);
        heic_error(decoder->doc->ctx, HEIC_SEVERITY_ERROR,
                   "dav1d returned unexpected sequence sample %u",
                   (unsigned)sample);
        return -1;
    }
    sequence_apply_color(item, picture);
    cache->pictures[sample] = *picture;
    memset(picture, 0, sizeof(*picture));
    cache->picture_ready[sample] = 1;
    return 0;
}

static heic_frame *sequence_decode_track(
    heic_sequence_decoder *decoder, heic_sequence_cache *cache,
    const heic_sequence *seq, uint32_t frame_index, heic_abort *ab)
{
    heic_doc *doc = decoder->doc;
    heic_item item;
    uint32_t target, start, i, j;
    if (!cache || seq->sample_count != cache->sample_count
        || frame_index >= seq->frame_count
        || heic_container_get_item(&doc->container,
                                   seq->coded_item_id, &item) != 0
        || (!item.hvcc && !item.av1c))
        return NULL;
    target = seq->frame_samples[frame_index];
    if (target >= seq->sample_count) return NULL;

    if (!cache->initialized || target < cache->cache_start) {
        sequence_cache_clear(decoder, cache);
        start = target;
        while (start > 0 && !seq->samples[start].is_sync) start--;
        if (!seq->samples[start].is_sync) return NULL;
        cache->cache_start = start;
        cache->next_sample = start;
        cache->initialized = 1;
    }
    while (!cache->picture_ready[target]) {
        if (cache->next_sample < cache->sample_count) {
            const heic_sequence_sample *sample;
            heic_frame *decoded;
            i = cache->next_sample;
            sample = &seq->samples[i];
            if (heic_abort_check(ab)) return NULL;
            if (i > cache->cache_start && sample->is_sync) {
                sequence_cache_clear(decoder, cache);
                cache->cache_start = i;
                cache->next_sample = i;
                cache->initialized = 1;
            }
            decoded = &cache->pictures[i];
            memset(decoded, 0, sizeof(*decoded));
            if (item.hvcc) {
                uint32_t n_refs = i - cache->cache_start;
                for (j = 0; j < n_refs; j++) {
                    if (!cache->picture_ready[i - 1 - j]) return NULL;
                    cache->refs[j] = &cache->pictures[i - 1 - j];
                }
                if (heic_hevc_decode_refs(
                        doc->ctx, item.hvcc,
                        doc->data + sample->offset, sample->size,
                        cache->refs, (int)n_refs, decoded, ab) != 0) {
                    heic_frame_free(doc->ctx, decoded);
                    cache->next_sample = i;
                    return NULL;
                }
                sequence_apply_color(&item, decoded);
                cache->picture_ready[i] = 1;
                cache->next_sample = i + 1;
            } else {
                heic_frame picture;
                uint32_t output_sample = 0;
                int got;
                if (!cache->av1)
                    cache->av1 = heic_av1_sequence_new(doc->ctx);
                if (!cache->av1) return NULL;
                memset(&picture, 0, sizeof(picture));
                got = heic_av1_sequence_submit(
                    cache->av1, item.av1c,
                    doc->data + sample->offset, sample->size, i,
                    &picture, &output_sample, ab);
                if (got < 0) {
                    heic_av1_sequence_destroy(cache->av1);
                    cache->av1 = NULL;
                    cache->next_sample = i;
                    return NULL;
                }
                cache->next_sample = i + 1;
                if (got > 0
                    && sequence_store_av1_picture(
                           decoder, cache, &item, &picture,
                           output_sample) != 0)
                    return NULL;
            }
        } else if (item.av1c) {
            heic_frame picture;
            uint32_t output_sample = 0;
            int got;
            memset(&picture, 0, sizeof(picture));
            got = heic_av1_sequence_receive(
                cache->av1, &picture, &output_sample, ab);
            if (got <= 0) {
                if (!heic_abort_check(ab))
                    heic_error(doc->ctx, HEIC_SEVERITY_ERROR,
                               "dav1d did not output sequence sample %u",
                               (unsigned)target);
                return NULL;
            }
            if (sequence_store_av1_picture(
                    decoder, cache, &item, &picture, output_sample) != 0)
                return NULL;
        } else {
            return NULL;
        }
    }
    return &cache->pictures[target];
}

heic_image *heic_sequence_decoder_decode_frame_abortable(
    heic_sequence_decoder *decoder, uint32_t frame_index, heic_abort *ab)
{
    const heic_sequence *seq;
    heic_frame *color;
    if (!decoder || !decoder->doc
        || !(seq = decoder->doc->container.sequence))
        return NULL;
    color = sequence_decode_track(
        decoder, &decoder->color, seq, frame_index, ab);
    if (!color) return NULL;
    if (seq->alpha
        && (decoder->format == HEIC_FORMAT_RGBA
            || decoder->format == HEIC_FORMAT_BGRA)
        && !color->a) {
        heic_frame *alpha = sequence_decode_track(
            decoder, &decoder->alpha, seq->alpha, frame_index, ab);
        if (!alpha
            || attach_decoded_alpha(decoder->doc->ctx, color, alpha) != 0)
            return NULL;
    }
    return sequence_frame_to_image(
        decoder->doc->ctx, color, decoder->format);
}

heic_image *heic_sequence_decoder_decode_frame(
    heic_sequence_decoder *decoder, uint32_t frame_index)
{
    return heic_sequence_decoder_decode_frame_abortable(
        decoder, frame_index, NULL);
}

heic_image *heic_doc_decode_sequence_frame_abortable(
    heic_doc *doc, uint32_t frame_index, heic_format format, heic_abort *ab)
{
    heic_sequence_decoder *decoder =
        heic_sequence_decoder_new(doc, format);
    heic_image *img;
    if (!decoder) return NULL;
    img = heic_sequence_decoder_decode_frame_abortable(
        decoder, frame_index, ab);
    heic_sequence_decoder_destroy(decoder);
    return img;
}

heic_image *heic_doc_decode_sequence_frame(heic_doc *doc,
                                           uint32_t frame_index,
                                           heic_format format)
{
    return heic_doc_decode_sequence_frame_abortable(
        doc, frame_index, format, NULL);
}

int heic_doc_decode_into(heic_doc *doc, heic_format format,
                         uint8_t *buf, size_t buf_size, int stride)
{
    return heic_decode_primary(doc, format, NULL, buf, buf_size, stride, NULL);
}

static heic_image *decode_item_to_image(heic_doc *doc, const heic_item *item,
                                        heic_format format,
                                        const heic_abort *ab)
{
    heic_frame frame;
    heic_image *img;
    int bpp;
    size_t need;
    int w, h;

    memset(&frame, 0, sizeof(frame));
    if (decode_item(doc, item, &frame, ab, 0) != 0) {
        heic_frame_free(doc->ctx, &frame);
        return NULL;
    }
    w = frame_cropped_w(&frame);
    h = frame_cropped_h(&frame);
    bpp = (format == HEIC_FORMAT_RGBA || format == HEIC_FORMAT_BGRA) ? 4 : 3;
    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / (size_t)bpp
        || (size_t)w * (size_t)bpp > SIZE_MAX / (size_t)h) {
        heic_frame_free(doc->ctx, &frame);
        return NULL;
    }
    need = (size_t)w * (size_t)h * (size_t)bpp;
    img = (heic_image *)heic_zalloc(doc->ctx, sizeof(heic_image));
    if (!img) {
        heic_frame_free(doc->ctx, &frame);
        return NULL;
    }
    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    img->format = format;
    img->stride = w * bpp;
    img->data = (uint8_t *)doc->ctx->alloc(doc->ctx->user, doc->ctx, need);
    if (!img->data || heic_frame_to_rgb(doc->ctx, &frame, format, img->data, img->stride) != 0) {
        heic_image_destroy(doc->ctx, img);
        heic_frame_free(doc->ctx, &frame);
        return NULL;
    }
    heic_frame_free(doc->ctx, &frame);
    return img;
}

heic_image *heic_doc_decode_thumbnail(heic_doc *doc, heic_format format)
{
    uint32_t thumbs[8];
    heic_item item;
    int n;
    if (!doc) return NULL;
    n = heic_container_find_thumbs(&doc->container,
                                   doc->container.primary_item_id, thumbs, 8);
    if (n <= 0
        || heic_container_get_item(&doc->container, thumbs[0], &item) != 0)
        return NULL;
    return decode_item_to_image(doc, &item, format, NULL);
}

heic_image *heic_doc_decode_gain_map_abortable(
    heic_doc *doc, heic_format format, heic_abort *ab)
{
    static const char gain_map_urn[] =
        "urn:com:apple:photo:2020:aux:hdrgainmap";
    uint32_t aux[8];
    heic_item item;
    int n;
    if (!doc) return NULL;
    n = heic_container_find_aux(&doc->container,
                                doc->container.primary_item_id,
                                gain_map_urn, aux, 8);
    if (n <= 0
        || heic_container_get_item(&doc->container, aux[0], &item) != 0)
        return NULL;
    return decode_item_to_image(doc, &item, format, ab);
}

heic_image *heic_doc_decode_gain_map(heic_doc *doc, heic_format format)
{
    return heic_doc_decode_gain_map_abortable(doc, format, NULL);
}
