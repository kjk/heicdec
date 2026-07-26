/* document.c -- heic_doc open, probe, metadata */
#include "heic_internal.h"

static heic_kind brand_kind(const heic_container *c)
{
    int i;
    heic_fourcc brands[16];
    int n = 0;
    brands[n++] = c->brand;
    for (i = 0; i < c->n_compatible_brands && n < 16; i++)
        brands[n++] = c->compatible_brands[i];
    for (i = 0; i < n; i++) {
        if (brands[i] == HEIC_FCC('a', 'v', 'i', 'f') ||
            brands[i] == HEIC_FCC('a', 'v', 'i', 's'))
            return HEIC_KIND_AVIF;
    }
    for (i = 0; i < n; i++) {
        if (brands[i] == HEIC_FCC('h', 'e', 'i', 'c') ||
            brands[i] == HEIC_FCC('h', 'e', 'i', 'x') ||
            brands[i] == HEIC_FCC('h', 'e', 'v', 'c') ||
            brands[i] == HEIC_FCC('h', 'e', 'v', 'x'))
            return HEIC_KIND_HEIC;
    }
    for (i = 0; i < n; i++) {
        if (brands[i] == HEIC_FCC('m', 's', 'f', '1'))
            return HEIC_KIND_SEQUENCE;
    }
    return HEIC_KIND_HEIF;
}

heic_doc *heic_doc_open(heic_ctx *ctx, const uint8_t *data, size_t len)
{
    heic_doc *doc;
    heic_init();
    if (!ctx || !data || len == 0) return NULL;
    doc = (heic_doc *)heic_zalloc(ctx, sizeof(heic_doc));
    if (!doc) return NULL;
    doc->ctx = ctx;
    doc->data = data;
    doc->len = len;
    if (heic_container_parse(ctx, data, len, &doc->container, NULL) != 0) {
        heic_free_buf(ctx, doc);
        return NULL;
    }
    doc->kind = brand_kind(&doc->container);
    return doc;
}

void heic_doc_close(heic_doc *doc)
{
    heic_ctx *ctx;
    if (!doc) return;
    ctx = doc->ctx;
    heic_container_free(&doc->container);
    heic_free_buf(ctx, doc);
}

heic_kind heic_doc_kind(const heic_doc *doc)
{
    return doc ? doc->kind : HEIC_KIND_UNKNOWN;
}

static int primary_item(const heic_doc *doc, heic_item *item)
{
    if (!doc) return -1;
    return heic_container_get_item(&doc->container, doc->container.primary_item_id, item);
}

/* Apply clap / 90° rotations to ispe dims (display size after transforms). */
static void apply_transform_dims(uint32_t *w, uint32_t *h, const heic_item *item)
{
    int i;
    for (i = 0; i < item->n_transforms; i++) {
        const heic_xform *t = &item->transforms[i];
        if (t->kind == HEIC_XFORM_IROT &&
            (t->irot.angle == 90 || t->irot.angle == 270)) {
            uint32_t tmp = *w;
            *w = *h;
            *h = tmp;
        } else if (t->kind == HEIC_XFORM_CLAP && t->clap.width_d && t->clap.height_d) {
            uint32_t cw = t->clap.width_n / t->clap.width_d;
            uint32_t ch = t->clap.height_n / t->clap.height_d;
            if (cw > 0 && ch > 0) {
                *w = cw;
                *h = ch;
            }
        }
    }
}

/* Max dimg chain length (cycle / pathological iden stacks). */
#define HEIC_RESOLVE_MAX_DEPTH 8

static int resolve_seen(const uint32_t *seen, int n_seen, uint32_t id)
{
    int i;
    for (i = 0; i < n_seen; i++)
        if (seen[i] == id) return 1;
    return 0;
}

/* Resolve display dimensions for grid/iden by following dimg. */
static int resolve_dims_r(const heic_doc *doc, const heic_item *item, uint32_t *w,
                          uint32_t *h, uint32_t *seen, int n_seen)
{
    if (item->has_dims) {
        *w = item->width;
        *h = item->height;
        apply_transform_dims(w, h, item);
        return 0;
    }
    if (n_seen >= HEIC_RESOLVE_MAX_DEPTH || resolve_seen(seen, n_seen, item->id))
        return -1;
    if (item->item_type == HEIC_TYPE_IDEN || item->item_type == HEIC_TYPE_TMAP ||
        item->item_type == HEIC_TYPE_GRID) {
        uint32_t refs[4];
        int n = heic_container_find_refs(&doc->container, item->id, HEIC_REF_DIMG, refs, 4);
        heic_item child;
        if (n >= 1 && heic_container_get_item(&doc->container, refs[0], &child) == 0) {
            seen[n_seen] = item->id;
            return resolve_dims_r(doc, &child, w, h, seen, n_seen + 1);
        }
    }
    return -1;
}

static int resolve_dims(const heic_doc *doc, const heic_item *item, uint32_t *w,
                        uint32_t *h)
{
    uint32_t seen[HEIC_RESOLVE_MAX_DEPTH];
    return resolve_dims_r(doc, item, w, h, seen, 0);
}

/* First dimg child with codec config (grid/iden primary often has ispe only). */
static int resolve_codec_item_r(const heic_doc *doc, const heic_item *item,
                                heic_item *out, uint32_t *seen, int n_seen)
{
    uint32_t refs[8];
    int n, i;
    if (item->hvcc || item->av1c) {
        *out = *item;
        return 0;
    }
    if (n_seen >= HEIC_RESOLVE_MAX_DEPTH || resolve_seen(seen, n_seen, item->id))
        return -1;
    if (item->item_type != HEIC_TYPE_GRID && item->item_type != HEIC_TYPE_IDEN &&
        item->item_type != HEIC_TYPE_TMAP)
        return -1;
    seen[n_seen] = item->id;
    n = heic_container_find_refs(&doc->container, item->id, HEIC_REF_DIMG, refs, 8);
    for (i = 0; i < n; i++) {
        heic_item child;
        if (heic_container_get_item(&doc->container, refs[i], &child) != 0) continue;
        if (resolve_codec_item_r(doc, &child, out, seen, n_seen + 1) == 0) return 0;
    }
    return -1;
}

static int resolve_codec_item(const heic_doc *doc, const heic_item *item, heic_item *out)
{
    uint32_t seen[HEIC_RESOLVE_MAX_DEPTH];
    return resolve_codec_item_r(doc, item, out, seen, 0);
}

static int bit_depth_from_item(const heic_item *item)
{
    if (item->hvcc) return 8 + item->hvcc->bit_depth_luma_minus8;
    if (item->av1c) {
        if (!item->av1c->high_bitdepth) return 8;
        return item->av1c->twelve_bit ? 12 : 10;
    }
    return 8;
}

int heic_doc_info(const heic_doc *doc, heic_image_info *info)
{
    heic_item item;
    heic_item codec;
    uint32_t w = 0, h = 0;
    uint32_t thumbs[4];
    uint32_t aux[4];
    int i;

    if (!doc || !info) return -1;
    memset(info, 0, sizeof(*info));
    info->full_range = -1;
    if (primary_item(doc, &item) != 0) return -1;
    if (resolve_dims(doc, &item, &w, &h) != 0) return -1;
    info->width = w;
    info->height = h;

    if (item.hvcc || item.av1c) {
        info->bit_depth = bit_depth_from_item(&item);
    } else if (resolve_codec_item(doc, &item, &codec) == 0) {
        info->bit_depth = bit_depth_from_item(&codec);
    } else
        info->bit_depth = 8;

    if (item.colr && item.colr->kind == HEIC_COLR_NCLX) {
        info->color_primaries = item.colr->color_primaries;
        info->transfer_characteristics = item.colr->transfer_characteristics;
        info->matrix_coefficients = item.colr->matrix_coefficients;
        info->full_range = item.colr->full_range ? 1 : 0;
    } else if (resolve_codec_item(doc, &item, &codec) == 0 && codec.colr &&
               codec.colr->kind == HEIC_COLR_NCLX) {
        info->color_primaries = codec.colr->color_primaries;
        info->transfer_characteristics = codec.colr->transfer_characteristics;
        info->matrix_coefficients = codec.colr->matrix_coefficients;
        info->full_range = codec.colr->full_range ? 1 : 0;
    }

    /* EXIF / XMP items */
    for (i = 0; i < doc->container.n_item_infos; i++) {
        heic_fourcc t = doc->container.item_infos[i].item_type;
        if (t == HEIC_TYPE_EXIF) info->has_exif = 1;
        if (t == HEIC_TYPE_MIME) {
            const char *ct = doc->container.item_infos[i].content_type;
            if (ct && (strstr(ct, "xmp") || strstr(ct, "rdf+xml")))
                info->has_xmp = 1;
        }
    }

    if (heic_container_find_thumbs(&doc->container, item.id, thumbs, 4) > 0)
        info->has_thumbnail = 1;

    /* alpha aux */
    if (heic_container_find_aux(&doc->container, item.id,
                                "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha",
                                aux, 4) > 0 ||
        heic_container_find_aux(&doc->container, item.id,
                                "urn:mpeg:hevc:2015:auxid:1", aux, 4) > 0)
        info->has_alpha = 1;

    if (heic_container_find_aux(&doc->container, item.id,
                                "urn:com:apple:photo:2020:aux:hdrgainmap",
                                aux, 4) > 0)
        info->has_gain_map = 1;

    return 0;
}

size_t heic_doc_output_size(const heic_doc *doc, heic_format format)
{
    heic_image_info info;
    int bpp;
    if (heic_doc_info(doc, &info) != 0) return 0;
    bpp = (format == HEIC_FORMAT_RGBA || format == HEIC_FORMAT_BGRA) ? 4 : 3;
    return (size_t)info.width * (size_t)info.height * (size_t)bpp;
}

int heic_doc_exif(heic_doc *doc, uint8_t **out, size_t *out_len)
{
    int i;
    if (!doc || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;
    for (i = 0; i < doc->container.n_item_infos; i++) {
        const heic_item_info *info = &doc->container.item_infos[i];
        const uint8_t *data = NULL;
        size_t len = 0;
        int owned = 0;
        uint8_t *copy;
        size_t payload;
        if (info->item_type != HEIC_TYPE_EXIF) continue;
        if (heic_container_item_data(&doc->container, info->item_id, &data, &len, &owned) != 0)
            continue;
        if (len <= 4) {
            if (owned) heic_free_buf(doc->ctx, (void *)data);
            continue;
        }
        /* Strip 4-byte TIFF offset prefix (HEIF). */
        payload = len - 4;
        copy = (uint8_t *)heic_zalloc(doc->ctx, payload);
        if (!copy) {
            if (owned) heic_free_buf(doc->ctx, (void *)data);
            return 0;
        }
        memcpy(copy, data + 4, payload);
        if (owned) heic_free_buf(doc->ctx, (void *)data);
        *out = copy;
        *out_len = payload;
        return 1;
    }
    return 0;
}

int heic_doc_xmp(heic_doc *doc, uint8_t **out, size_t *out_len)
{
    int i;
    if (!doc || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;
    for (i = 0; i < doc->container.n_item_infos; i++) {
        const heic_item_info *info = &doc->container.item_infos[i];
        const uint8_t *data = NULL;
        size_t len = 0;
        int owned = 0;
        uint8_t *copy;
        if (info->item_type != HEIC_TYPE_MIME) continue;
        if (!info->content_type) continue;
        if (!strstr(info->content_type, "xmp") && !strstr(info->content_type, "rdf+xml"))
            continue;
        if (heic_container_item_data(&doc->container, info->item_id, &data, &len, &owned) != 0)
            continue;
        copy = (uint8_t *)heic_zalloc(doc->ctx, len ? len : 1);
        if (!copy) {
            if (owned) heic_free_buf(doc->ctx, (void *)data);
            return 0;
        }
        if (len) memcpy(copy, data, len);
        if (owned) heic_free_buf(doc->ctx, (void *)data);
        *out = copy;
        *out_len = len;
        return 1;
    }
    return 0;
}

int heic_doc_icc(heic_doc *doc, uint8_t **out, size_t *out_len)
{
    heic_item item;
    int i, j;
    if (!doc || !out || !out_len) return 0;
    *out = NULL;
    *out_len = 0;
    if (primary_item(doc, &item) != 0) return 0;
    if (!item.colr || item.colr->kind != HEIC_COLR_ICC || !item.colr->icc) {
        /* An item may carry both ICC and nclx; get_item intentionally exposes
           the last color property for decode, so search all associations here. */
        for (i = 0; i < doc->container.n_property_associations; i++) {
            const heic_ipma *a = &doc->container.property_associations[i];
            if (a->item_id != item.id) continue;
            for (j = 0; j < a->n_props; j++) {
                uint16_t pi = a->prop_indices[j];
                const heic_property *p;
                if (!pi || pi > (uint16_t)doc->container.n_properties) continue;
                p = &doc->container.properties[pi - 1];
                if (p->kind == HEIC_PROP_COLR && p->colr.kind == HEIC_COLR_ICC
                    && p->colr.icc) {
                    item.colr = &p->colr;
                    break;
                }
            }
            if (item.colr && item.colr->kind == HEIC_COLR_ICC) break;
        }
    }
    if (!item.colr || item.colr->kind != HEIC_COLR_ICC || !item.colr->icc) return 0;
    {
        uint8_t *copy = (uint8_t *)heic_zalloc(doc->ctx, item.colr->icc_len ? item.colr->icc_len : 1);
        if (!copy) return 0;
        if (item.colr->icc_len) memcpy(copy, item.colr->icc, item.colr->icc_len);
        *out = copy;
        *out_len = item.colr->icc_len;
        return 1;
    }
}
