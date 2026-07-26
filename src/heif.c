/* heif.c -- ISOBMFF / HEIF container parser (port of imazen/heic heif module) */
#include "heic_internal.h"

/* ---- box iterator ---- */

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         offset;
} heic_box_iter;

typedef struct {
    heic_fourcc    type;
    uint64_t       size;          /* total including header */
    size_t         content_off;   /* absolute offset of content in file/parent */
    const uint8_t *content;
    size_t         content_len;
} heic_box;

static void box_iter_init(heic_box_iter *it, const uint8_t *data, size_t len)
{
    it->data = data;
    it->len = len;
    it->offset = 0;
}

static int box_iter_next(heic_box_iter *it, heic_box *out)
{
    size_t off, header_size;
    uint32_t size32;
    uint64_t size;
    heic_fourcc type;
    size_t size_usize, box_end;

    if (!it || !out) return 0;
    off = it->offset;
    if (off + 8 > it->len) return 0;

    size32 = heic_rb32(it->data + off);
    type = heic_read_fcc(it->data + off + 4);

    if (size32 == 1) {
        if (off + 16 > it->len) return 0;
        size = heic_rb64(it->data + off + 8);
        header_size = 16;
    } else if (size32 == 0) {
        size = (uint64_t)(it->len - off);
        header_size = 8;
    } else {
        size = size32;
        header_size = 8;
    }

    if (size < header_size) return 0;
    size_usize = (size_t)size;
    if ((uint64_t)size_usize != size) return 0;
    box_end = off + size_usize;
    if (box_end > it->len) return 0;

    out->type = type;
    out->size = size;
    out->content_off = off + header_size;
    out->content = it->data + off + header_size;
    out->content_len = size_usize - header_size;
    it->offset = box_end;
    return 1;
}

static uint64_t read_sized_int(const uint8_t *data, size_t len, size_t *pos, size_t size)
{
    uint64_t value = 0;
    size_t i;
    if (size == 0 || *pos + size > len) return 0;
    for (i = 0; i < size; i++) value = (value << 8) | data[*pos + i];
    *pos += size;
    return value;
}

static char *dup_cstr_z(heic_ctx *ctx, const uint8_t *p, size_t maxlen)
{
    size_t n = 0;
    char *s;
    while (n < maxlen && p[n] != 0) n++;
    if (n > HEIC_MAX_STRING_LEN) return NULL;
    s = (char *)heic_zalloc(ctx, n + 1);
    if (!s) return NULL;
    if (n) memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

static int is_heif_brand(heic_fourcc b)
{
    return b == HEIC_FCC('h', 'e', 'i', 'c') || b == HEIC_FCC('h', 'e', 'i', 'x') ||
           b == HEIC_FCC('h', 'e', 'v', 'c') || b == HEIC_FCC('h', 'e', 'v', 'x') ||
           b == HEIC_FCC('m', 'i', 'f', '1') || b == HEIC_FCC('m', 's', 'f', '1') ||
           b == HEIC_FCC('m', 'i', 'f', '2') || b == HEIC_FCC('m', 'i', 'f', '3') ||
           b == HEIC_FCC('a', 'v', 'i', 'f') || b == HEIC_FCC('a', 'v', 'i', 's');
}

static void free_hvcc(heic_ctx *ctx, heic_hvcc *h)
{
    int i;
    if (!h) return;
    for (i = 0; i < h->n_nal_units; i++) heic_free_buf(ctx, h->nal_units[i]);
    heic_free_buf(ctx, h->nal_units);
    heic_free_buf(ctx, h->nal_unit_lens);
    heic_free_buf(ctx, h->nal_blob);
    memset(h, 0, sizeof(*h));
}

static void free_property(heic_ctx *ctx, heic_property *p)
{
    if (!p) return;
    if (p->kind == HEIC_PROP_HVCC) free_hvcc(ctx, &p->hvcc);
    if (p->kind == HEIC_PROP_AV1C) {
        heic_free_buf(ctx, p->av1c.config_obus);
        memset(&p->av1c, 0, sizeof(p->av1c));
    }
    if (p->kind == HEIC_PROP_COLR) {
        heic_free_buf(ctx, p->colr.icc);
        memset(&p->colr, 0, sizeof(p->colr));
    }
    if (p->kind == HEIC_PROP_AUXC) {
        heic_free_buf(ctx, p->auxc.aux_type);
        heic_free_buf(ctx, p->auxc.subtype_data);
        memset(&p->auxc, 0, sizeof(p->auxc));
    }
    if (p->kind == HEIC_PROP_UNCC) {
        heic_free_buf(ctx, p->uncc.components);
        memset(&p->uncc, 0, sizeof(p->uncc));
    }
    if (p->kind == HEIC_PROP_CMPD) {
        heic_free_buf(ctx, p->cmpd.types);
        memset(&p->cmpd, 0, sizeof(p->cmpd));
    }
    if (p->kind == HEIC_PROP_ICEF) {
        heic_free_buf(ctx, p->icef.units);
        memset(&p->icef, 0, sizeof(p->icef));
    }
    p->kind = HEIC_PROP_UNKNOWN;
}

static void free_property_associations(heic_ctx *ctx, heic_container *c)
{
    int i;
    for (i = 0; i < c->n_property_associations; i++) {
        heic_free_buf(ctx, c->property_associations[i].prop_indices);
        heic_free_buf(ctx, c->property_associations[i].essential);
    }
    heic_free_buf(ctx, c->property_associations);
    c->property_associations = NULL;
    c->n_property_associations = 0;
}

static void free_item_locations(heic_ctx *ctx, heic_container *c)
{
    int i;
    for (i = 0; i < c->n_item_locations; i++)
        heic_free_buf(ctx, c->item_locations[i].extents);
    heic_free_buf(ctx, c->item_locations);
    c->item_locations = NULL;
    c->n_item_locations = 0;
}

static void free_item_infos(heic_ctx *ctx, heic_container *c)
{
    int i;
    for (i = 0; i < c->n_item_infos; i++) {
        heic_free_buf(ctx, c->item_infos[i].item_name);
        heic_free_buf(ctx, c->item_infos[i].content_type);
    }
    heic_free_buf(ctx, c->item_infos);
    c->item_infos = NULL;
    c->n_item_infos = 0;
}

void heic_container_free(heic_container *c)
{
    int i;
    heic_ctx *ctx;
    if (!c || !c->ctx) return;
    ctx = c->ctx;
    heic_free_buf(ctx, c->compatible_brands);
    free_item_locations(ctx, c);
    free_item_infos(ctx, c);
    for (i = 0; i < c->n_properties; i++) free_property(ctx, &c->properties[i]);
    heic_free_buf(ctx, c->properties);
    free_property_associations(ctx, c);
    for (i = 0; i < c->n_item_references; i++)
        heic_free_buf(ctx, c->item_references[i].to_item_ids);
    heic_free_buf(ctx, c->item_references);
    if (c->sequence) {
        heic_free_buf(ctx, c->sequence->samples);
        heic_free_buf(ctx, c->sequence->frame_samples);
        heic_free_buf(ctx, c->sequence->frame_times);
        heic_free_buf(ctx, c->sequence->frame_durations);
        heic_free_buf(ctx, c->sequence);
    }
    memset(c, 0, sizeof(*c));
}

static int parse_ftyp(heic_ctx *ctx, const heic_box *b, heic_container *c)
{
    size_t pos;
    int n = 0;
    if (b->content_len < 8) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "ftyp too short");
        return -1;
    }
    c->brand = heic_read_fcc(b->content);
    c->minor_brand = heic_read_fcc(b->content + 4);
    pos = 8;
    while (pos + 4 <= b->content_len && n < HEIC_MAX_COMPAT_BRANDS) n++, pos += 4;
    if (n > 0) {
        int i;
        c->compatible_brands = (heic_fourcc *)heic_zalloc(ctx, (size_t)n * sizeof(heic_fourcc));
        if (!c->compatible_brands) return -1;
        pos = 8;
        for (i = 0; i < n; i++, pos += 4)
            c->compatible_brands[i] = heic_read_fcc(b->content + pos);
        c->n_compatible_brands = n;
    }
    if (!is_heif_brand(c->brand)) {
        int i, ok = 0;
        for (i = 0; i < c->n_compatible_brands; i++)
            if (is_heif_brand(c->compatible_brands[i])) { ok = 1; break; }
        if (!ok) {
            heic_error(ctx, HEIC_SEVERITY_ERROR, "not a HEIF/AVIF file");
            return -1;
        }
    }
    return 0;
}

/* ---- MinimizedImageBox (ISO/IEC 23008-12 Annex O) ---- */

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t bit_pos;
    int error;
} heic_mini_bits;

static uint32_t mini_bits(heic_mini_bits *b, int n)
{
    uint32_t v = 0;
    int i;
    if (!b || n < 0 || n > 32 || b->bit_pos > b->len * 8u
        || (size_t)n > b->len * 8u - b->bit_pos) {
        if (b) b->error = 1;
        return 0;
    }
    for (i = 0; i < n; i++) {
        size_t p = b->bit_pos++;
        v = (v << 1) | ((b->data[p >> 3] >> (7 - (p & 7))) & 1);
    }
    return v;
}

static heic_fourcc mini_fcc(heic_mini_bits *b)
{
    uint8_t a = (uint8_t)mini_bits(b, 8);
    uint8_t c = (uint8_t)mini_bits(b, 8);
    uint8_t d = (uint8_t)mini_bits(b, 8);
    uint8_t e = (uint8_t)mini_bits(b, 8);
    return HEIC_FCC(a, c, d, e);
}

static int mini_add_property(heic_container *c, const heic_property *p)
{
    c->properties[c->n_properties] = *p;
    return ++c->n_properties; /* 1-based property index */
}

static int mini_make_assoc(heic_ctx *ctx, heic_ipma *a, uint32_t item_id,
                           const uint16_t *indices, int n)
{
    int i;
    memset(a, 0, sizeof(*a));
    a->item_id = item_id;
    a->prop_indices = (uint16_t *)heic_zalloc(ctx, (size_t)n * sizeof(uint16_t));
    a->essential = (uint8_t *)heic_zalloc(ctx, (size_t)n);
    if ((!a->prop_indices || !a->essential) && n) return -1;
    for (i = 0; i < n; i++) {
        a->prop_indices[i] = indices[i];
        a->essential[i] = 1;
    }
    a->n_props = n;
    return 0;
}

static void mini_set_orientation_props(heic_container *c, uint8_t orientation,
                                       uint16_t *main_props, int *n_main,
                                       uint16_t *alpha_props, int *n_alpha)
{
    heic_property p;
    int rotation = 0, mirror = -1, idx;
    switch (orientation) {
    case 2: mirror = 1; break; /* horizontal */
    case 3: rotation = 180; break;
    case 4: mirror = 0; break; /* vertical */
    case 5: rotation = 90; mirror = 1; break;
    case 6: rotation = 90; break;
    case 7: rotation = 90; mirror = 0; break;
    case 8: rotation = 270; break;
    default: break;
    }
    if (rotation) {
        memset(&p, 0, sizeof(p));
        p.kind = HEIC_PROP_IROT;
        p.irot.angle = (uint16_t)rotation;
        idx = mini_add_property(c, &p);
        main_props[(*n_main)++] = (uint16_t)idx;
        if (alpha_props) alpha_props[(*n_alpha)++] = (uint16_t)idx;
    }
    if (mirror >= 0) {
        memset(&p, 0, sizeof(p));
        p.kind = HEIC_PROP_IMIR;
        p.imir.axis = (uint8_t)mirror;
        idx = mini_add_property(c, &p);
        main_props[(*n_main)++] = (uint16_t)idx;
        if (alpha_props) alpha_props[(*n_alpha)++] = (uint16_t)idx;
    }
}

static int mini_make_location(heic_ctx *ctx, heic_item_loc *loc, uint32_t id,
                              size_t offset, uint32_t length)
{
    memset(loc, 0, sizeof(*loc));
    loc->item_id = id;
    loc->extents = (heic_extent *)heic_zalloc(ctx, sizeof(heic_extent));
    if (!loc->extents) return -1;
    loc->extents[0].offset = offset;
    loc->extents[0].length = length;
    loc->n_extents = 1;
    return 0;
}

static int parse_hvcc(heic_ctx *ctx, const heic_box *b, heic_hvcc *out);
static int parse_av1c(heic_ctx *ctx, const heic_box *b, heic_av1c *out);

static int parse_mini(heic_ctx *ctx, const heic_box *box, heic_container *c)
{
    heic_mini_bits bits;
    uint8_t version, explicit_codecs, float_flag, full_range, alpha_flag;
    uint8_t explicit_cicp, hdr_flag, icc_flag, exif_flag, xmp_flag;
    uint8_t chroma, orientation, bit_depth = 8;
    uint8_t primaries, transfer, matrix;
    uint32_t width, height, main_config_size, main_data_size;
    uint32_t alpha_config_size = 0, alpha_data_size = 0;
    uint32_t icc_size = 0, exif_size = 0, xmp_size = 0;
    int large_dims, large_metadata = 0, large_config, large_data, legacy_flags = 0;
    int metadata_compressed = 0;
    heic_fourcc item_type, config_type = 0;
    size_t pos, dimension_bits_pos, main_config_off, alpha_config_off = 0, icc_off = 0;
    size_t alpha_data_off = 0, main_data_off, exif_off = 0, xmp_off = 0;
    uint64_t required;
    int n_items, n_locs, n_assocs, n_refs, info_i = 0, loc_i = 0, ref_i = 0;
    uint16_t main_props[10], alpha_props[10];
    int n_main_props = 0, n_alpha_props = 0;
    heic_property p;
    heic_box cfg_box;
    int idx, main_cfg_idx, ispe_idx, alpha_cfg_idx = 0;

    memset(&bits, 0, sizeof(bits));
    bits.data = box->content;
    bits.len = box->content_len;
    version = (uint8_t)mini_bits(&bits, 2);
    explicit_codecs = (uint8_t)mini_bits(&bits, 1);
    float_flag = (uint8_t)mini_bits(&bits, 1);
    full_range = (uint8_t)mini_bits(&bits, 1);
    alpha_flag = (uint8_t)mini_bits(&bits, 1);
    explicit_cicp = (uint8_t)mini_bits(&bits, 1);
    hdr_flag = (uint8_t)mini_bits(&bits, 1);
    icc_flag = (uint8_t)mini_bits(&bits, 1);
    exif_flag = (uint8_t)mini_bits(&bits, 1);
    xmp_flag = (uint8_t)mini_bits(&bits, 1);
    chroma = (uint8_t)mini_bits(&bits, 2);
    orientation = (uint8_t)mini_bits(&bits, 3) + 1;
    large_dims = (int)mini_bits(&bits, 1);
    dimension_bits_pos = bits.bit_pos;
    width = mini_bits(&bits, large_dims ? 15 : 7) + 1;
    height = mini_bits(&bits, large_dims ? 15 : 7) + 1;
    /* Early draft files inverted large_dimensions_flag. The old libheif
       lightning fixture signals the 15-bit maximum, which is not a useful
       real dimension; accept that unambiguous draft encoding. */
    if (large_dims && (width == 32768 || height == 32768
                       || width == 32767 || height == 32767)) {
        bits.bit_pos = dimension_bits_pos;
        bits.error = 0;
        large_dims = 0;
        legacy_flags = 1;
        width = mini_bits(&bits, 7) + 1;
        height = mini_bits(&bits, 7) + 1;
    }

    if (version != 0 || float_flag || hdr_flag) {
        heic_error(ctx, HEIC_SEVERITY_ERROR,
                   "unsupported mini features (version, float, or HDR gain map)");
        return -1;
    }
    if (!width || !height || width > ctx->limits.max_width
        || height > ctx->limits.max_height
        || (uint64_t)width * height > ctx->limits.max_pixels)
        return -1;

    if (chroma == 1 || chroma == 2) (void)mini_bits(&bits, 1);
    if (chroma == 1) (void)mini_bits(&bits, 1);
    if (mini_bits(&bits, 1)) bit_depth = (uint8_t)mini_bits(&bits, 3) + 9;
    if (alpha_flag) (void)mini_bits(&bits, 1); /* premultiplied */

    if (explicit_cicp) {
        primaries = (uint8_t)mini_bits(&bits, 8);
        transfer = (uint8_t)mini_bits(&bits, 8);
        matrix = (uint8_t)mini_bits(&bits, 8);
    } else {
        primaries = icc_flag ? 2 : 1;
        transfer = icc_flag ? 2 : 13;
        matrix = chroma == 0 ? 2 : 6;
    }

    item_type = c->minor_brand;
    if (explicit_codecs) {
        item_type = mini_fcc(&bits);
        config_type = mini_fcc(&bits);
    } else if (item_type == HEIC_FCC('h', 'e', 'i', 'c')
               || item_type == HEIC_FCC('h', 'e', 'i', 'x')) {
        item_type = HEIC_TYPE_HVC1;
        config_type = HEIC_BOX_HVCC;
    } else if (item_type == HEIC_FCC('a', 'v', 'i', 'f')
               || item_type == HEIC_FCC('a', 'v', 'i', 's')) {
        item_type = HEIC_TYPE_AV01;
        config_type = HEIC_BOX_AV1C;
    }
    if ((item_type != HEIC_TYPE_HVC1 && item_type != HEIC_TYPE_AV01)
        || (config_type != HEIC_BOX_HVCC && config_type != HEIC_BOX_AV1C)) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "mini codec brand is unsupported");
        return -1;
    }

    if (icc_flag || exif_flag || xmp_flag) large_metadata = (int)mini_bits(&bits, 1);
    large_config = (int)mini_bits(&bits, 1);
    large_data = (int)mini_bits(&bits, 1);
    if (legacy_flags) {
        large_metadata = !large_metadata;
        large_config = !large_config;
        large_data = !large_data;
    }
    if (icc_flag) icc_size = mini_bits(&bits, large_metadata ? 20 : 10) + 1;
    main_config_size = mini_bits(&bits, large_config ? 12 : 3);
    main_data_size = mini_bits(&bits, large_data ? 28 : 15) + 1;
    if (alpha_flag) alpha_data_size = mini_bits(&bits, large_data ? 28 : 15);
    if (alpha_flag && alpha_data_size)
        alpha_config_size = mini_bits(&bits, large_config ? 12 : 3);
    if (exif_flag || xmp_flag) metadata_compressed = (int)mini_bits(&bits, 1);
    if (exif_flag) exif_size = mini_bits(&bits, large_metadata ? 20 : 10) + 1;
    if (xmp_flag) xmp_size = mini_bits(&bits, large_metadata ? 20 : 10) + 1;
    if (bits.error) return -1;
    bits.bit_pos = (bits.bit_pos + 7) & ~(size_t)7;
    pos = bits.bit_pos / 8;

    required = (uint64_t)main_config_size + alpha_config_size + icc_size
             + alpha_data_size + main_data_size + exif_size + xmp_size;
    if (pos > box->content_len || required > box->content_len - pos)
        return -1;
    if (icc_size > HEIC_MAX_ICC_SIZE) return -1;
    if (metadata_compressed) {
        heic_error(ctx, HEIC_SEVERITY_WARNING,
                   "compressed mini EXIF/XMP is not exposed");
        exif_flag = xmp_flag = 0;
        exif_size = xmp_size = 0;
    }

    main_config_off = pos; pos += main_config_size;
    if (alpha_flag && alpha_data_size) {
        alpha_config_off = pos;
        pos += alpha_config_size;
    }
    if (icc_flag) {
        icc_off = pos;
        pos += icc_size;
    }
    if (alpha_flag && alpha_data_size) {
        alpha_data_off = pos;
        pos += alpha_data_size;
    }
    main_data_off = pos; pos += main_data_size;
    if (exif_flag) { exif_off = pos; pos += exif_size; }
    if (xmp_flag) { xmp_off = pos; pos += xmp_size; }

    n_items = 1 + (alpha_flag && alpha_data_size ? 1 : 0)
                + (exif_flag ? 1 : 0) + (xmp_flag ? 1 : 0);
    n_locs = n_items;
    n_assocs = 1 + (alpha_flag && alpha_data_size ? 1 : 0);
    n_refs = (alpha_flag && alpha_data_size ? 1 : 0)
             + (exif_flag ? 1 : 0) + (xmp_flag ? 1 : 0);
    c->item_infos = (heic_item_info *)heic_zalloc(
        ctx, (size_t)n_items * sizeof(heic_item_info));
    c->item_locations = (heic_item_loc *)heic_zalloc(
        ctx, (size_t)n_locs * sizeof(heic_item_loc));
    c->properties = (heic_property *)heic_zalloc(ctx, 10 * sizeof(heic_property));
    c->property_associations = (heic_ipma *)heic_zalloc(
        ctx, (size_t)n_assocs * sizeof(heic_ipma));
    c->item_references = (heic_iref *)heic_zalloc(
        ctx, (size_t)n_refs * sizeof(heic_iref));
    if (!c->item_infos || !c->item_locations || !c->properties
        || !c->property_associations || (n_refs && !c->item_references))
        return -1;
    /* Publish zero-initialized capacities immediately so container_free also
       releases nested allocations if expansion fails partway through. */
    c->n_item_infos = n_items;
    c->n_item_locations = n_locs;
    c->n_property_associations = n_assocs;
    c->n_item_references = n_refs;

    c->primary_item_id = 1;
    c->item_infos[info_i].item_id = 1;
    c->item_infos[info_i++].item_type = item_type;
    if (mini_make_location(ctx, &c->item_locations[loc_i++], 1,
                           box->content_off + main_data_off, main_data_size) != 0)
        return -1;

    memset(&cfg_box, 0, sizeof(cfg_box));
    cfg_box.content = box->content + main_config_off;
    cfg_box.content_len = main_config_size;
    memset(&p, 0, sizeof(p));
    if (item_type == HEIC_TYPE_HVC1) {
        if (parse_hvcc(ctx, &cfg_box, &p.hvcc) != 0) return -1;
        p.kind = HEIC_PROP_HVCC;
    } else {
        if (parse_av1c(ctx, &cfg_box, &p.av1c) != 0) return -1;
        p.kind = HEIC_PROP_AV1C;
    }
    main_cfg_idx = mini_add_property(c, &p);
    main_props[n_main_props++] = (uint16_t)main_cfg_idx;

    memset(&p, 0, sizeof(p));
    p.kind = HEIC_PROP_ISPE;
    p.ispe.width = width;
    p.ispe.height = height;
    ispe_idx = mini_add_property(c, &p);
    main_props[n_main_props++] = (uint16_t)ispe_idx;

    if (icc_flag) {
        memset(&p, 0, sizeof(p));
        p.kind = HEIC_PROP_COLR;
        p.colr.kind = HEIC_COLR_ICC;
        p.colr.icc_len = icc_size;
        p.colr.icc = (uint8_t *)heic_zalloc(ctx, icc_size ? icc_size : 1);
        if (!p.colr.icc) return -1;
        memcpy(p.colr.icc, box->content + icc_off, icc_size);
        idx = mini_add_property(c, &p);
        main_props[n_main_props++] = (uint16_t)idx;
    }
    memset(&p, 0, sizeof(p));
    p.kind = HEIC_PROP_COLR;
    p.colr.kind = HEIC_COLR_NCLX;
    p.colr.color_primaries = primaries;
    p.colr.transfer_characteristics = transfer;
    p.colr.matrix_coefficients = matrix;
    p.colr.full_range = full_range;
    idx = mini_add_property(c, &p);
    main_props[n_main_props++] = (uint16_t)idx;

    if (alpha_flag && alpha_data_size) {
        c->item_infos[info_i].item_id = 2;
        c->item_infos[info_i].item_type = item_type;
        c->item_infos[info_i++].hidden = 1;
        if (mini_make_location(ctx, &c->item_locations[loc_i++], 2,
                               box->content_off + alpha_data_off,
                               alpha_data_size) != 0)
            return -1;
        cfg_box.content = alpha_config_size
                              ? box->content + alpha_config_off
                              : box->content + main_config_off;
        cfg_box.content_len = alpha_config_size ? alpha_config_size : main_config_size;
        memset(&p, 0, sizeof(p));
        if (item_type == HEIC_TYPE_HVC1) {
            if (parse_hvcc(ctx, &cfg_box, &p.hvcc) != 0) return -1;
            p.kind = HEIC_PROP_HVCC;
        } else {
            if (parse_av1c(ctx, &cfg_box, &p.av1c) != 0) return -1;
            p.kind = HEIC_PROP_AV1C;
        }
        alpha_cfg_idx = mini_add_property(c, &p);
        alpha_props[n_alpha_props++] = (uint16_t)alpha_cfg_idx;
        alpha_props[n_alpha_props++] = (uint16_t)ispe_idx;
        memset(&p, 0, sizeof(p));
        p.kind = HEIC_PROP_AUXC;
        p.auxc.aux_type = dup_cstr_z(
            ctx, (const uint8_t *)"urn:mpeg:mpegB:cicp:systems:auxiliary:alpha",
            strlen("urn:mpeg:mpegB:cicp:systems:auxiliary:alpha") + 1);
        if (!p.auxc.aux_type) return -1;
        idx = mini_add_property(c, &p);
        alpha_props[n_alpha_props++] = (uint16_t)idx;
    }

    mini_set_orientation_props(c, orientation, main_props, &n_main_props,
                               alpha_flag && alpha_data_size ? alpha_props : NULL,
                               &n_alpha_props);
    if (mini_make_assoc(ctx, &c->property_associations[0], 1,
                        main_props, n_main_props) != 0)
        return -1;
    if (alpha_flag && alpha_data_size) {
        if (mini_make_assoc(ctx, &c->property_associations[1], 2,
                            alpha_props, n_alpha_props) != 0)
            return -1;
        c->item_references[ref_i].ref_type = HEIC_REF_AUXL;
        c->item_references[ref_i].from_item_id = 2;
        c->item_references[ref_i].to_item_ids =
            (uint32_t *)heic_zalloc(ctx, sizeof(uint32_t));
        if (!c->item_references[ref_i].to_item_ids) return -1;
        c->item_references[ref_i].to_item_ids[0] = 1;
        c->item_references[ref_i++].n_to = 1;
    }

    if (exif_flag) {
        c->item_infos[info_i].item_id = 6;
        c->item_infos[info_i++].item_type = HEIC_TYPE_EXIF;
        if (mini_make_location(ctx, &c->item_locations[loc_i++], 6,
                               box->content_off + exif_off, exif_size) != 0)
            return -1;
    }
    if (xmp_flag) {
        static const uint8_t xmp_type[] = "application/rdf+xml";
        c->item_infos[info_i].item_id = 7;
        c->item_infos[info_i].item_type = HEIC_TYPE_MIME;
        c->item_infos[info_i].content_type =
            dup_cstr_z(ctx, xmp_type, sizeof(xmp_type));
        if (!c->item_infos[info_i].content_type) return -1;
        info_i++;
        if (mini_make_location(ctx, &c->item_locations[loc_i++], 7,
                               box->content_off + xmp_off, xmp_size) != 0)
            return -1;
    }
    while (ref_i < n_refs) {
        uint32_t from = exif_flag ? 6u : 7u;
        if (exif_flag) exif_flag = 0; else xmp_flag = 0;
        c->item_references[ref_i].ref_type = HEIC_REF_CDSC;
        c->item_references[ref_i].from_item_id = from;
        c->item_references[ref_i].to_item_ids =
            (uint32_t *)heic_zalloc(ctx, sizeof(uint32_t));
        if (!c->item_references[ref_i].to_item_ids) return -1;
        c->item_references[ref_i].to_item_ids[0] = 1;
        c->item_references[ref_i++].n_to = 1;
    }

    c->has_meta = 1;
    (void)bit_depth; /* codec config remains authoritative for decode */
    return 0;
}

static int parse_pitm(heic_ctx *ctx, const heic_box *b, heic_container *c)
{
    uint8_t version;
    if (b->content_len < 6) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "pitm too short");
        return -1;
    }
    version = b->content[0];
    if (version == 0)
        c->primary_item_id = heic_rb16(b->content + 4);
    else {
        if (b->content_len < 8) return -1;
        c->primary_item_id = heic_rb32(b->content + 4);
    }
    return 0;
}

static int parse_iloc(heic_ctx *ctx, const heic_box *b, heic_container *c,
                      const heic_abort *ab)
{
    const uint8_t *content = b->content;
    size_t len = b->content_len;
    uint8_t version, offset_size, length_size, base_offset_size, index_size;
    size_t pos;
    uint32_t item_count, i;

    if (len < 8) return -1;
    version = content[0];
    offset_size = (content[4] >> 4) & 0xF;
    length_size = content[4] & 0xF;
    base_offset_size = (content[5] >> 4) & 0xF;
    index_size = version >= 1 ? (content[5] & 0xF) : 0;
    pos = 6;

    if (version < 2) {
        if (pos + 2 > len) return -1;
        item_count = heic_rb16(content + pos);
        pos += 2;
    } else {
        if (pos + 4 > len) return -1;
        item_count = heic_rb32(content + pos);
        pos += 4;
    }
    if (item_count > HEIC_MAX_ITEMS) return -1;

    /* Replace prior iloc (duplicate box / second meta) — index only into the
     * freshly allocated buffer so a stale n_item_locations cannot OOB. */
    free_item_locations(ctx, c);
    c->item_locations = (heic_item_loc *)heic_zalloc(ctx, item_count * sizeof(heic_item_loc));
    if (!c->item_locations && item_count) return -1;

    for (i = 0; i < item_count; i++) {
        heic_item_loc *loc;
        uint16_t extent_count;
        uint32_t e;
        if (heic_abort_check(ab)) return -1;
        loc = &c->item_locations[i];
        if (version < 2) {
            if (pos + 2 > len) break;
            loc->item_id = heic_rb16(content + pos);
            pos += 2;
        } else {
            if (pos + 4 > len) break;
            loc->item_id = heic_rb32(content + pos);
            pos += 4;
        }
        if (version >= 1) {
            if (pos + 2 > len) break;
            loc->construction_method = content[pos + 1] & 0xF;
            pos += 2;
        } else
            loc->construction_method = 0;
        if (pos + 2 > len) break;
        pos += 2; /* data_reference_index */
        loc->base_offset = read_sized_int(content, len, &pos, base_offset_size);
        if (pos + 2 > len) break;
        extent_count = heic_rb16(content + pos);
        pos += 2;
        if (extent_count > HEIC_MAX_EXTENTS_PER_ITEM) return -1;
        loc->extents = (heic_extent *)heic_zalloc(ctx, extent_count * sizeof(heic_extent));
        if (!loc->extents && extent_count) return -1;
        loc->n_extents = extent_count;
        for (e = 0; e < extent_count; e++) {
            if (index_size) (void)read_sized_int(content, len, &pos, index_size);
            loc->extents[e].offset = read_sized_int(content, len, &pos, offset_size);
            loc->extents[e].length = read_sized_int(content, len, &pos, length_size);
        }
        c->n_item_locations = (int)(i + 1);
    }
    return 0;
}

static int parse_infe(heic_ctx *ctx, const heic_box *b, heic_item_info *info)
{
    const uint8_t *content = b->content;
    size_t len = b->content_len;
    uint8_t version;
    uint32_t flags;
    size_t pos;
    size_t name_end, ct_end;

    memset(info, 0, sizeof(*info));
    if (len < 8) return -1;
    version = content[0];
    flags = ((uint32_t)content[1] << 16) | ((uint32_t)content[2] << 8) | content[3];
    info->hidden = (flags & 1) != 0;
    pos = 4;
    if (version < 3) {
        if (pos + 2 > len) return -1;
        info->item_id = heic_rb16(content + pos);
        pos += 2;
    } else {
        if (pos + 4 > len) return -1;
        info->item_id = heic_rb32(content + pos);
        pos += 4;
    }
    if (pos + 2 > len) return -1;
    pos += 2; /* protection_index */
    if (version >= 2) {
        if (pos + 4 > len) return -1;
        info->item_type = heic_read_fcc(content + pos);
        pos += 4;
    }
    if (pos >= len) {
        info->item_name = (char *)heic_zalloc(ctx, 1);
        info->content_type = (char *)heic_zalloc(ctx, 1);
        return 0;
    }
    name_end = 0;
    while (pos + name_end < len && content[pos + name_end] != 0) name_end++;
    if (name_end > HEIC_MAX_STRING_LEN) return -1;
    info->item_name = dup_cstr_z(ctx, content + pos, name_end + 1);
    pos += name_end + (pos + name_end < len ? 1 : 0);
    ct_end = 0;
    if (pos < len) {
        while (pos + ct_end < len && content[pos + ct_end] != 0) ct_end++;
        if (ct_end > HEIC_MAX_STRING_LEN) return -1;
        info->content_type = dup_cstr_z(ctx, content + pos, ct_end + 1);
    } else
        info->content_type = (char *)heic_zalloc(ctx, 1);
    if (!info->item_name || !info->content_type) return -1;
    return 0;
}

static int parse_iinf(heic_ctx *ctx, const heic_box *b, heic_container *c,
                      const heic_abort *ab)
{
    const uint8_t *content = b->content;
    size_t len = b->content_len;
    uint8_t version;
    size_t pos;
    uint32_t entry_count;
    heic_box_iter it;
    heic_box child;

    if (len < 6) return -1;
    version = content[0];
    pos = 4;
    if (version == 0) {
        if (pos + 2 > len) return -1;
        entry_count = heic_rb16(content + pos);
        pos += 2;
    } else {
        if (pos + 4 > len) return -1;
        entry_count = heic_rb32(content + pos);
        pos += 4;
    }
    if (entry_count > HEIC_MAX_ITEMS) return -1;
    free_item_infos(ctx, c);
    c->item_infos = (heic_item_info *)heic_zalloc(ctx, entry_count * sizeof(heic_item_info));
    if (!c->item_infos && entry_count) return -1;

    box_iter_init(&it, content + pos, len - pos);
    while (box_iter_next(&it, &child)) {
        heic_item_info info;
        if (heic_abort_check(ab)) return -1;
        if (child.type != HEIC_BOX_INFE) continue;
        if (parse_infe(ctx, &child, &info) != 0) continue;
        if ((uint32_t)c->n_item_infos >= entry_count) break;
        c->item_infos[c->n_item_infos++] = info;
    }
    return 0;
}

static int parse_ispe(const heic_box *b, heic_ispe *out)
{
    if (b->content_len < 12) return -1;
    out->width = heic_rb32(b->content + 4);
    out->height = heic_rb32(b->content + 8);
    if (out->width == 0 || out->height == 0) return -1;
    return 0;
}

static int parse_hvcc(heic_ctx *ctx, const heic_box *b, heic_hvcc *out)
{
    const uint8_t *content = b->content;
    size_t len = b->content_len;
    size_t pos;
    uint8_t num_arrays, a;
    int n_nals = 0, cap = 0;
    uint8_t **nals = NULL;
    size_t *nal_lens = NULL;

    memset(out, 0, sizeof(*out));
    if (len < 23) return -1;
    out->config_version = content[0];
    out->general_profile_space = (content[1] >> 6) & 3;
    out->general_tier_flag = (content[1] >> 5) & 1;
    out->general_profile_idc = content[1] & 0x1F;
    out->general_profile_compatibility_flags = heic_rb32(content + 2);
    out->general_constraint_indicator_flags =
        ((uint64_t)content[6] << 40) | ((uint64_t)content[7] << 32) |
        ((uint64_t)content[8] << 24) | ((uint64_t)content[9] << 16) |
        ((uint64_t)content[10] << 8) | (uint64_t)content[11];
    out->general_level_idc = content[12];
    out->chroma_format = content[16] & 3;
    out->bit_depth_luma_minus8 = content[17] & 7;
    out->bit_depth_chroma_minus8 = content[18] & 7;
    out->length_size_minus_one = content[21] & 3;
    if (out->length_size_minus_one == 2) return -1;
    num_arrays = content[22];
    pos = 23;

    for (a = 0; a < num_arrays; a++) {
        uint16_t num_nalus, n;
        if (pos + 3 > len) break;
        pos += 1; /* nal type */
        num_nalus = heic_rb16(content + pos);
        pos += 2;
        for (n = 0; n < num_nalus; n++) {
            uint16_t nalu_len;
            uint8_t *copy;
            if (pos + 2 > len) break;
            nalu_len = heic_rb16(content + pos);
            pos += 2;
            if (pos + (size_t)nalu_len > len) break;
            if (n_nals >= cap) {
                int ncap = cap ? cap * 2 : 8;
                uint8_t **nn = (uint8_t **)heic_realloc_buf(ctx, nals,
                    (size_t)cap * sizeof(uint8_t *), (size_t)ncap * sizeof(uint8_t *));
                size_t *nl = (size_t *)heic_realloc_buf(ctx, nal_lens,
                    (size_t)cap * sizeof(size_t), (size_t)ncap * sizeof(size_t));
                if (!nn || !nl) {
                    heic_free_buf(ctx, nn);
                    heic_free_buf(ctx, nl);
                    goto fail;
                }
                nals = nn;
                nal_lens = nl;
                cap = ncap;
            }
            copy = (uint8_t *)heic_zalloc(ctx, nalu_len);
            if (!copy) goto fail;
            memcpy(copy, content + pos, nalu_len);
            nals[n_nals] = copy;
            nal_lens[n_nals] = nalu_len;
            n_nals++;
            pos += nalu_len;
        }
    }
    out->nal_units = nals;
    out->nal_unit_lens = nal_lens;
    out->n_nal_units = n_nals;
    return 0;
fail:
    {
        int i;
        for (i = 0; i < n_nals; i++) heic_free_buf(ctx, nals[i]);
        heic_free_buf(ctx, nals);
        heic_free_buf(ctx, nal_lens);
    }
    return -1;
}

static int parse_av1c(heic_ctx *ctx, const heic_box *b, heic_av1c *out)
{
    const uint8_t *c = b->content;
    size_t len = b->content_len;
    memset(out, 0, sizeof(*out));
    if (len < 4) return -1;
    /* marker|version should be 0x81 */
    out->seq_profile = (c[1] >> 5) & 7;
    out->seq_level_idx_0 = c[1] & 0x1F;
    out->high_bitdepth = (c[2] >> 6) & 1;
    out->twelve_bit = (c[2] >> 5) & 1;
    out->monochrome = (c[2] >> 4) & 1;
    out->chroma_subsampling_x = (c[2] >> 3) & 1;
    out->chroma_subsampling_y = (c[2] >> 2) & 1;
    if (len > 4) {
        out->config_obus_len = len - 4;
        out->config_obus = (uint8_t *)heic_zalloc(ctx, out->config_obus_len);
        if (!out->config_obus) return -1;
        memcpy(out->config_obus, c + 4, out->config_obus_len);
    }
    return 0;
}

static int parse_colr(heic_ctx *ctx, const heic_box *b, heic_colr *out)
{
    heic_fourcc ct;
    memset(out, 0, sizeof(*out));
    if (b->content_len < 4) return -1;
    ct = heic_read_fcc(b->content);
    if (ct == HEIC_FCC('n', 'c', 'l', 'x')) {
        if (b->content_len < 11) return -1;
        out->kind = HEIC_COLR_NCLX;
        out->color_primaries = heic_rb16(b->content + 4);
        out->transfer_characteristics = heic_rb16(b->content + 6);
        out->matrix_coefficients = heic_rb16(b->content + 8);
        out->full_range = (b->content[10] >> 7) != 0;
        return 0;
    }
    if (ct == HEIC_FCC('p', 'r', 'o', 'f') || ct == HEIC_FCC('r', 'i', 'c', 'c')) {
        size_t icc_len = b->content_len - 4;
        if (icc_len > HEIC_MAX_ICC_SIZE) return -1;
        out->kind = HEIC_COLR_ICC;
        out->icc_len = icc_len;
        out->icc = (uint8_t *)heic_zalloc(ctx, icc_len ? icc_len : 1);
        if (!out->icc) return -1;
        if (icc_len) memcpy(out->icc, b->content + 4, icc_len);
        return 0;
    }
    return -1;
}

static int parse_clap(const heic_box *b, heic_clap *out)
{
    const uint8_t *c = b->content;
    if (b->content_len < 32) return -1;
    out->width_n = heic_rb32(c + 0);
    out->width_d = heic_rb32(c + 4);
    out->height_n = heic_rb32(c + 8);
    out->height_d = heic_rb32(c + 12);
    out->horiz_off_n = (int32_t)heic_rb32(c + 16);
    out->horiz_off_d = heic_rb32(c + 20);
    out->vert_off_n = (int32_t)heic_rb32(c + 24);
    out->vert_off_d = heic_rb32(c + 28);
    if (!out->width_d || !out->height_d || !out->horiz_off_d || !out->vert_off_d)
        return -1;
    return 0;
}

static int parse_irot(const heic_box *b, heic_irot *out)
{
    if (b->content_len < 1) return -1;
    switch (b->content[0] & 3) {
    case 0: out->angle = 0; break;
    case 1: out->angle = 270; break; /* 90 CCW */
    case 2: out->angle = 180; break;
    case 3: out->angle = 90; break;
    }
    return 0;
}

static int parse_imir(const heic_box *b, heic_imir *out)
{
    if (b->content_len < 1) return -1;
    out->axis = b->content[0] & 1;
    return 0;
}

static int parse_auxc(heic_ctx *ctx, const heic_box *b, heic_auxc *out)
{
    const uint8_t *data;
    size_t dlen, end;
    memset(out, 0, sizeof(*out));
    if (b->content_len < 5) return -1;
    data = b->content + 4;
    dlen = b->content_len - 4;
    end = 0;
    while (end < dlen && data[end] != 0) end++;
    if (end > HEIC_MAX_STRING_LEN) return -1;
    out->aux_type = dup_cstr_z(ctx, data, end + 1);
    if (!out->aux_type) return -1;
    if (end + 1 < dlen) {
        out->subtype_len = dlen - (end + 1);
        out->subtype_data = (uint8_t *)heic_zalloc(ctx, out->subtype_len);
        if (!out->subtype_data) return -1;
        memcpy(out->subtype_data, data + end + 1, out->subtype_len);
    }
    return 0;
}

/* uncC FullBox (ISO 23001-17) — imazen parse_uncc layout. */
static int parse_uncc(heic_ctx *ctx, const heic_box *b, heic_uncc *out)
{
    const uint8_t *c = b->content;
    size_t len = b->content_len, pos = 4;
    uint32_t ncomp, i;

    memset(out, 0, sizeof(*out));
    if (len < 12) return -1;
    out->profile = heic_rb32(c + pos);
    pos += 4;
    ncomp = heic_rb32(c + pos);
    pos += 4;
    if (ncomp == 0 || ncomp > 16) return -1;
    out->components =
        (heic_uncc_comp *)heic_zalloc(ctx, ncomp * sizeof(heic_uncc_comp));
    if (!out->components) return -1;
    out->n_components = (int)ncomp;
    for (i = 0; i < ncomp; i++) {
        if (pos + 5 > len) {
            heic_free_buf(ctx, out->components);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->components[i].component_index = heic_rb16(c + pos);
        pos += 2;
        out->components[i].component_bit_depth_minus_one = c[pos++];
        out->components[i].component_format = c[pos++];
        out->components[i].component_align_size = c[pos++];
    }
    if (pos + 4 <= len) {
        uint8_t flags;
        out->sampling_type = c[pos++];
        out->interleave_type = c[pos++];
        out->block_size = c[pos++];
        flags = c[pos++];
        out->components_little_endian = (flags & 0x80) ? 1 : 0;
        out->block_pad_lsb = (flags & 0x40) ? 1 : 0;
        out->block_little_endian = (flags & 0x20) ? 1 : 0;
        out->block_reversed = (flags & 0x10) ? 1 : 0;
        out->pad_unknown = (flags & 0x08) ? 1 : 0;
    }
    /* pixel_size is uint32 in ISO 23001-17 / libheif (not uint8). */
    if (pos + 4 <= len) {
        out->pixel_size = heic_rb32(c + pos);
        pos += 4;
    }
    if (pos + 4 <= len) {
        out->row_align_size = heic_rb32(c + pos);
        pos += 4;
    }
    if (pos + 4 <= len) {
        out->tile_align_size = heic_rb32(c + pos);
        pos += 4;
    }
    if (pos + 4 <= len) {
        out->num_tile_cols_minus_one = heic_rb32(c + pos);
        pos += 4;
    }
    if (pos + 4 <= len) out->num_tile_rows_minus_one = heic_rb32(c + pos);
    return 0;
}

static int parse_cmpc(const heic_box *b, heic_cmpc *out)
{
    memset(out, 0, sizeof(*out));
    /* FullBox: version/flags (4) + compression_type (4) + unit_type (1) */
    if (b->content_len < 9) return -1;
    out->compression_type = heic_read_fcc(b->content + 4);
    out->unit_type = b->content[8];
    if (out->unit_type > 4) return -1;
    return 0;
}

/* icef unit field widths (ISO 23001-17): code → bits. */
static const uint8_t icef_offset_bits[] = {0, 16, 24, 32, 64};
static const uint8_t icef_size_bits[] = {8, 16, 24, 32, 64};

static uint64_t icef_read_bits(const uint8_t *p, size_t len, size_t *bitpos, int nbits)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < nbits; i++) {
        size_t bp = *bitpos + (size_t)i;
        size_t byte = bp / 8;
        int bit = 7 - (int)(bp % 8);
        if (byte >= len) return 0;
        v = (v << 1) | (uint64_t)((p[byte] >> bit) & 1);
    }
    *bitpos += (size_t)nbits;
    return v;
}

static int parse_icef(heic_ctx *ctx, const heic_box *b, heic_icef *out)
{
    const uint8_t *c;
    size_t len, bitpos;
    uint8_t codes, off_code, sz_code;
    uint32_t n, i;
    uint64_t implied = 0;
    int off_bits, sz_bits;

    memset(out, 0, sizeof(*out));
    /* FullBox header already included in content for our parser: ver/flags at [0..3] */
    if (b->content_len < 9) return -1;
    c = b->content + 4;
    len = b->content_len - 4;
    codes = c[0];
    off_code = (uint8_t)((codes >> 5) & 7);
    sz_code = (uint8_t)((codes >> 2) & 7);
    if (off_code > 4 || sz_code > 4) return -1;
    off_bits = (int)icef_offset_bits[off_code];
    sz_bits = (int)icef_size_bits[sz_code];
    if (len < 5) return -1;
    n = heic_rb32(c + 1);
    if (n > 1000000) return -1;
    bitpos = 40; /* 1 byte codes + 4 byte count, in bits */
    if (n == 0) return 0;
    out->units = (heic_icef_unit *)heic_zalloc(ctx, (size_t)n * sizeof(heic_icef_unit));
    if (!out->units) return -1;
    out->n_units = (int)n;
    for (i = 0; i < n; i++) {
        uint64_t off, sz;
        if (off_code == 0)
            off = implied;
        else
            off = icef_read_bits(c, len, &bitpos, off_bits);
        sz = icef_read_bits(c, len, &bitpos, sz_bits);
        if (bitpos / 8 > len) {
            heic_free_buf(ctx, out->units);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->units[i].offset = off;
        out->units[i].size = sz;
        if (off_code == 0) implied += sz;
    }
    return 0;
}

static int parse_cmpd(heic_ctx *ctx, const heic_box *b, heic_cmpd *out)
{
    const uint8_t *c = b->content;
    size_t len = b->content_len, pos = 0;
    uint32_t n, i;

    memset(out, 0, sizeof(*out));
    if (len < 4) return -1;
    n = heic_rb32(c);
    pos = 4;
    if (n == 0 || n > 16) return -1;
    out->types = (uint16_t *)heic_zalloc(ctx, n * sizeof(uint16_t));
    if (!out->types) return -1;
    out->n_types = (int)n;
    for (i = 0; i < n; i++) {
        if (pos + 2 > len) {
            heic_free_buf(ctx, out->types);
            memset(out, 0, sizeof(*out));
            return -1;
        }
        out->types[i] = heic_rb16(c + pos);
        pos += 2;
        if (out->types[i] >= 0x8000) {
            /* URI string after type — skip to NUL */
            while (pos < len && c[pos]) pos++;
            if (pos < len) pos++;
        }
    }
    return 0;
}

static int parse_ipco(heic_ctx *ctx, const heic_box *b, heic_container *c,
                      const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    heic_property *props = NULL;
    int n = 0, cap = 0;

    box_iter_init(&it, b->content, b->content_len);
    while (box_iter_next(&it, &child)) {
        heic_property prop;
        if (heic_abort_check(ab)) goto fail;
        if ((uint32_t)n >= HEIC_MAX_PROPERTIES) goto fail;
        memset(&prop, 0, sizeof(prop));
        prop.kind = HEIC_PROP_UNKNOWN;
        if (child.type == HEIC_BOX_ISPE) {
            if (parse_ispe(&child, &prop.ispe) == 0) prop.kind = HEIC_PROP_ISPE;
        } else if (child.type == HEIC_BOX_HVCC || child.type == HEIC_BOX_HVCB) {
            if (parse_hvcc(ctx, &child, &prop.hvcc) == 0) prop.kind = HEIC_PROP_HVCC;
        } else if (child.type == HEIC_BOX_AV1C) {
            if (parse_av1c(ctx, &child, &prop.av1c) == 0) prop.kind = HEIC_PROP_AV1C;
        } else if (child.type == HEIC_BOX_COLR) {
            if (parse_colr(ctx, &child, &prop.colr) == 0) prop.kind = HEIC_PROP_COLR;
        } else if (child.type == HEIC_BOX_CLAP) {
            if (parse_clap(&child, &prop.clap) == 0) prop.kind = HEIC_PROP_CLAP;
        } else if (child.type == HEIC_BOX_IROT) {
            if (parse_irot(&child, &prop.irot) == 0) prop.kind = HEIC_PROP_IROT;
        } else if (child.type == HEIC_BOX_IMIR) {
            if (parse_imir(&child, &prop.imir) == 0) prop.kind = HEIC_PROP_IMIR;
        } else if (child.type == HEIC_BOX_AUXC) {
            if (parse_auxc(ctx, &child, &prop.auxc) == 0) prop.kind = HEIC_PROP_AUXC;
        } else if (child.type == HEIC_BOX_UNCC) {
            if (parse_uncc(ctx, &child, &prop.uncc) == 0) prop.kind = HEIC_PROP_UNCC;
        } else if (child.type == HEIC_BOX_CMPC) {
            if (parse_cmpc(&child, &prop.cmpc) == 0) prop.kind = HEIC_PROP_CMPC;
        } else if (child.type == HEIC_BOX_CMPD) {
            if (parse_cmpd(ctx, &child, &prop.cmpd) == 0) prop.kind = HEIC_PROP_CMPD;
        } else if (child.type == HEIC_BOX_ICEF) {
            if (parse_icef(ctx, &child, &prop.icef) == 0) prop.kind = HEIC_PROP_ICEF;
        }
        if (n >= cap) {
            int ncap = cap ? cap * 2 : 16;
            heic_property *np = (heic_property *)heic_realloc_buf(
                ctx, props, (size_t)cap * sizeof(heic_property),
                (size_t)ncap * sizeof(heic_property));
            if (!np) goto fail;
            props = np;
            cap = ncap;
        }
        props[n++] = prop;
    }
    c->properties = props;
    c->n_properties = n;
    return 0;
fail:
    {
        int i;
        for (i = 0; i < n; i++) free_property(ctx, &props[i]);
        heic_free_buf(ctx, props);
    }
    return -1;
}

static int parse_ipma(heic_ctx *ctx, const heic_box *b, heic_container *c,
                      const heic_abort *ab)
{
    const uint8_t *content = b->content;
    size_t len = b->content_len;
    uint8_t version;
    uint32_t flags, entry_count, e;
    size_t pos;

    if (len < 8) return -1;
    version = content[0];
    flags = ((uint32_t)content[1] << 16) | ((uint32_t)content[2] << 8) | content[3];
    pos = 4;
    entry_count = heic_rb32(content + pos);
    pos += 4;
    if (entry_count > HEIC_MAX_ITEMS) return -1;

    /* Replace prior associations (duplicate ipma / second meta). Always index
     * into a freshly allocated [0, entry_count) buffer — never append using a
     * stale n_property_associations (heap OOB on re-entry). */
    free_property_associations(ctx, c);
    c->property_associations =
        (heic_ipma *)heic_zalloc(ctx, entry_count * sizeof(heic_ipma));
    if (!c->property_associations && entry_count) return -1;

    for (e = 0; e < entry_count; e++) {
        heic_ipma *a;
        uint8_t assoc_count, k;
        if (heic_abort_check(ab)) return -1;
        a = &c->property_associations[e];
        if (version < 1) {
            if (pos + 2 > len) break;
            a->item_id = heic_rb16(content + pos);
            pos += 2;
        } else {
            if (pos + 4 > len) break;
            a->item_id = heic_rb32(content + pos);
            pos += 4;
        }
        if (pos >= len) break;
        assoc_count = content[pos++];
        a->prop_indices = (uint16_t *)heic_zalloc(ctx, assoc_count * sizeof(uint16_t));
        a->essential = (uint8_t *)heic_zalloc(ctx, assoc_count);
        if ((!a->prop_indices || !a->essential) && assoc_count) return -1;
        a->n_props = 0;
        for (k = 0; k < assoc_count; k++) {
            if (flags & 1) {
                uint16_t val;
                if (pos + 2 > len) break;
                val = heic_rb16(content + pos);
                pos += 2;
                a->essential[a->n_props] = (val >> 15) != 0;
                a->prop_indices[a->n_props] = val & 0x7FFF;
            } else {
                uint8_t val;
                if (pos >= len) break;
                val = content[pos++];
                a->essential[a->n_props] = (val >> 7) != 0;
                a->prop_indices[a->n_props] = val & 0x7F;
            }
            a->n_props++;
        }
        c->n_property_associations = (int)(e + 1);
    }
    return 0;
}

static int parse_iprp(heic_ctx *ctx, const heic_box *b, heic_container *c,
                      const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    box_iter_init(&it, b->content, b->content_len);
    while (box_iter_next(&it, &child)) {
        if (heic_abort_check(ab)) return -1;
        if (child.type == HEIC_BOX_IPCO) {
            if (parse_ipco(ctx, &child, c, ab) != 0) return -1;
        } else if (child.type == HEIC_BOX_IPMA) {
            if (parse_ipma(ctx, &child, c, ab) != 0) return -1;
        }
    }
    return 0;
}

static int parse_iref(heic_ctx *ctx, const heic_box *b, heic_container *c,
                      const heic_abort *ab)
{
    const uint8_t *content = b->content;
    size_t len = b->content_len;
    uint8_t version;
    heic_box_iter it;
    heic_box child;
    heic_iref *refs = NULL;
    int n = 0, cap = 0;

    if (len < 4) return -1;
    version = content[0];
    box_iter_init(&it, content + 4, len - 4);
    while (box_iter_next(&it, &child)) {
        const uint8_t *data = child.content;
        size_t dlen = child.content_len;
        size_t pos = 0;
        size_t id_size = version == 0 ? 2u : 4u;
        while (pos < dlen) {
            heic_iref r;
            uint16_t ref_count, t;
            if (heic_abort_check(ab)) goto fail;
            if ((uint32_t)n >= HEIC_MAX_REFERENCES) goto fail;
            memset(&r, 0, sizeof(r));
            r.ref_type = child.type;
            if (pos + id_size > dlen) break;
            r.from_item_id = id_size == 2 ? heic_rb16(data + pos) : heic_rb32(data + pos);
            pos += id_size;
            if (pos + 2 > dlen) break;
            ref_count = heic_rb16(data + pos);
            pos += 2;
            if (ref_count > HEIC_MAX_REFS_PER_ENTRY) goto fail;
            r.to_item_ids = (uint32_t *)heic_zalloc(ctx, ref_count * sizeof(uint32_t));
            if (!r.to_item_ids && ref_count) goto fail;
            for (t = 0; t < ref_count; t++) {
                if (pos + id_size > dlen) break;
                r.to_item_ids[r.n_to++] =
                    id_size == 2 ? heic_rb16(data + pos) : heic_rb32(data + pos);
                pos += id_size;
            }
            if (n >= cap) {
                int ncap = cap ? cap * 2 : 8;
                heic_iref *nr = (heic_iref *)heic_realloc_buf(
                    ctx, refs, (size_t)cap * sizeof(heic_iref),
                    (size_t)ncap * sizeof(heic_iref));
                if (!nr) {
                    heic_free_buf(ctx, r.to_item_ids);
                    goto fail;
                }
                refs = nr;
                cap = ncap;
            }
            refs[n++] = r;
        }
    }
    c->item_references = refs;
    c->n_item_references = n;
    return 0;
fail:
    {
        int i;
        for (i = 0; i < n; i++) heic_free_buf(ctx, refs[i].to_item_ids);
        heic_free_buf(ctx, refs);
    }
    return -1;
}

/* ---- image sequences (moov/trak) ---- */

#define HEIC_SEQ_MAX_TRACKS 16
#define HEIC_SEQ_MAX_ENTRIES HEIC_MAX_ITEMS

typedef struct {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t sample_desc_idx;
} heic_seq_stsc;

typedef struct {
    uint32_t count;
    uint32_t delta;
} heic_seq_stts;

typedef struct {
    uint32_t count;
    int64_t offset;
} heic_seq_ctts;

typedef struct {
    uint64_t segment_duration;
    int64_t media_time;
    int16_t rate_integer;
    int16_t rate_fraction;
} heic_seq_edit;

typedef struct {
    uint32_t track_id;
    uint32_t width, height;
    heic_fourcc handler_type;
    uint32_t media_timescale;
    uint64_t media_duration;
    heic_hvcc hvcc;
    heic_colr colr;
    int has_hvcc;
    int has_colr;
    uint32_t uniform_sample_size;
    uint32_t sample_count;
    uint32_t *sample_sizes;
    uint64_t *chunk_offsets;
    uint32_t n_chunk_offsets;
    heic_seq_stsc *sample_to_chunk;
    uint32_t n_sample_to_chunk;
    uint32_t *sync_samples;
    uint32_t n_sync_samples;
    heic_seq_stts *time_to_sample;
    uint32_t n_time_to_sample;
    heic_seq_ctts *composition_offsets;
    uint32_t n_composition_offsets;
    heic_seq_edit *edits;
    uint32_t n_edits;
    int edit_repeat;
} heic_seq_track;

static void seq_free_track(heic_ctx *ctx, heic_seq_track *t)
{
    if (!t) return;
    if (t->has_hvcc) free_hvcc(ctx, &t->hvcc);
    if (t->has_colr) heic_free_buf(ctx, t->colr.icc);
    heic_free_buf(ctx, t->sample_sizes);
    heic_free_buf(ctx, t->chunk_offsets);
    heic_free_buf(ctx, t->sample_to_chunk);
    heic_free_buf(ctx, t->sync_samples);
    heic_free_buf(ctx, t->time_to_sample);
    heic_free_buf(ctx, t->composition_offsets);
    heic_free_buf(ctx, t->edits);
    memset(t, 0, sizeof(*t));
}

static int seq_parse_duration_header(const heic_box *b, uint32_t *timescale,
                                     uint64_t *duration)
{
    const uint8_t *p = b->content;
    if (b->content_len < 4) return -1;
    if (p[0] == 0) {
        if (b->content_len < 20) return -1;
        *timescale = heic_rb32(p + 12);
        *duration = heic_rb32(p + 16);
    } else if (p[0] == 1) {
        if (b->content_len < 32) return -1;
        *timescale = heic_rb32(p + 20);
        *duration = heic_rb64(p + 24);
    } else {
        return -1;
    }
    return *timescale ? 0 : -1;
}

static int seq_parse_mdhd(const heic_box *b, heic_seq_track *t)
{
    return seq_parse_duration_header(b, &t->media_timescale,
                                     &t->media_duration);
}

static int seq_parse_elst(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t)
{
    uint32_t count, i;
    size_t pos, unit;
    int version;
    if (b->content_len < 8 || t->edits) return -1;
    version = b->content[0];
    if (version != 0 && version != 1) return -1;
    t->edit_repeat = (b->content[3] & 1) != 0;
    count = heic_rb32(b->content + 4);
    unit = version ? 20u : 12u;
    if (!count || count > HEIC_SEQ_MAX_ENTRIES
        || (size_t)count > (b->content_len - 8) / unit)
        return -1;
    t->edits = (heic_seq_edit *)heic_zalloc(
        ctx, (size_t)count * sizeof(heic_seq_edit));
    if (!t->edits) return -1;
    t->n_edits = count;
    pos = 8;
    for (i = 0; i < count; i++, pos += unit) {
        heic_seq_edit *e = &t->edits[i];
        if (version) {
            e->segment_duration = heic_rb64(b->content + pos);
            e->media_time = (int64_t)heic_rb64(b->content + pos + 8);
            e->rate_integer = (int16_t)heic_rb16(b->content + pos + 16);
            e->rate_fraction = (int16_t)heic_rb16(b->content + pos + 18);
        } else {
            e->segment_duration = heic_rb32(b->content + pos);
            e->media_time = (int32_t)heic_rb32(b->content + pos + 4);
            e->rate_integer = (int16_t)heic_rb16(b->content + pos + 8);
            e->rate_fraction = (int16_t)heic_rb16(b->content + pos + 10);
        }
    }
    return 0;
}

static int seq_parse_edts(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t)
{
    heic_box_iter it;
    heic_box child;
    box_iter_init(&it, b->content, b->content_len);
    while (box_iter_next(&it, &child)) {
        if (child.type == HEIC_BOX_ELST)
            return seq_parse_elst(ctx, &child, t);
    }
    return 0;
}

static int seq_parse_tkhd(const heic_box *b, heic_seq_track *t)
{
    const uint8_t *p = b->content;
    if (b->content_len < 4) return -1;
    if (p[0] == 0) {
        if (b->content_len < 84) return -1;
        t->track_id = heic_rb32(p + 12);
        t->width = heic_rb32(p + 76) >> 16;
        t->height = heic_rb32(p + 80) >> 16;
    } else if (p[0] == 1) {
        if (b->content_len < 96) return -1;
        t->track_id = heic_rb32(p + 20);
        t->width = heic_rb32(p + 88) >> 16;
        t->height = heic_rb32(p + 92) >> 16;
    } else {
        return -1;
    }
    return 0;
}

static int seq_parse_hdlr(const heic_box *b, heic_seq_track *t)
{
    if (b->content_len < 12) return -1;
    t->handler_type = heic_read_fcc(b->content + 8);
    return 0;
}

static int seq_parse_visual_entry(heic_ctx *ctx, const uint8_t *data,
                                  size_t len, heic_seq_track *t,
                                  const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    if (len < 86) return -1;
    if (!t->width) t->width = heic_rb16(data + 32);
    if (!t->height) t->height = heic_rb16(data + 34);
    box_iter_init(&it, data + 86, len - 86);
    while (box_iter_next(&it, &child)) {
        if (heic_abort_check(ab)) return -1;
        if (child.type == HEIC_BOX_HVCC && !t->has_hvcc) {
            if (parse_hvcc(ctx, &child, &t->hvcc) != 0) return -1;
            t->has_hvcc = 1;
        } else if (child.type == HEIC_BOX_COLR && !t->has_colr) {
            if (parse_colr(ctx, &child, &t->colr) == 0)
                t->has_colr = 1;
        }
    }
    return 0;
}

static int seq_parse_stsd(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t, const heic_abort *ab)
{
    uint32_t count, i;
    size_t pos = 8;
    if (b->content_len < 8) return -1;
    count = heic_rb32(b->content + 4);
    if (count > 16) count = 16;
    for (i = 0; i < count; i++) {
        uint32_t size;
        heic_fourcc type;
        if (heic_abort_check(ab)) return -1;
        if (pos + 8 > b->content_len) return -1;
        size = heic_rb32(b->content + pos);
        type = heic_read_fcc(b->content + pos + 4);
        if (size < 8 || size > b->content_len - pos) return -1;
        if (type == HEIC_BOX_HVC1 || type == HEIC_BOX_HEV1)
            return seq_parse_visual_entry(ctx, b->content + pos, size, t, ab);
        pos += size;
    }
    return 0;
}

static int seq_parse_stsz(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t)
{
    uint32_t i;
    size_t needed;
    if (b->content_len < 12 || t->sample_count || t->sample_sizes
        || t->uniform_sample_size)
        return -1;
    t->uniform_sample_size = heic_rb32(b->content + 4);
    t->sample_count = heic_rb32(b->content + 8);
    if (!t->sample_count || t->sample_count > HEIC_SEQ_MAX_ENTRIES) return -1;
    if (t->uniform_sample_size) return 0;
    needed = 12 + (size_t)t->sample_count * 4;
    if (needed > b->content_len) return -1;
    t->sample_sizes = (uint32_t *)heic_zalloc(
        ctx, (size_t)t->sample_count * sizeof(uint32_t));
    if (!t->sample_sizes) return -1;
    for (i = 0; i < t->sample_count; i++)
        t->sample_sizes[i] = heic_rb32(b->content + 12 + (size_t)i * 4);
    return 0;
}

static int seq_parse_chunk_offsets(heic_ctx *ctx, const heic_box *b,
                                   heic_seq_track *t, int is_64)
{
    uint32_t count, i;
    size_t unit = is_64 ? 8u : 4u;
    size_t needed;
    if (b->content_len < 8 || t->chunk_offsets) return -1;
    count = heic_rb32(b->content + 4);
    if (!count || count > HEIC_SEQ_MAX_ENTRIES) return -1;
    needed = 8 + (size_t)count * unit;
    if (needed > b->content_len) return -1;
    t->chunk_offsets = (uint64_t *)heic_zalloc(
        ctx, (size_t)count * sizeof(uint64_t));
    if (!t->chunk_offsets) return -1;
    t->n_chunk_offsets = count;
    for (i = 0; i < count; i++) {
        const uint8_t *p = b->content + 8 + (size_t)i * unit;
        t->chunk_offsets[i] = is_64 ? heic_rb64(p) : heic_rb32(p);
    }
    return 0;
}

static int seq_parse_stsc(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t)
{
    uint32_t count, i;
    size_t needed;
    if (b->content_len < 8 || t->sample_to_chunk) return -1;
    count = heic_rb32(b->content + 4);
    if (!count || count > HEIC_SEQ_MAX_ENTRIES) return -1;
    needed = 8 + (size_t)count * 12;
    if (needed > b->content_len) return -1;
    t->sample_to_chunk = (heic_seq_stsc *)heic_zalloc(
        ctx, (size_t)count * sizeof(heic_seq_stsc));
    if (!t->sample_to_chunk) return -1;
    t->n_sample_to_chunk = count;
    for (i = 0; i < count; i++) {
        const uint8_t *p = b->content + 8 + (size_t)i * 12;
        t->sample_to_chunk[i].first_chunk = heic_rb32(p);
        t->sample_to_chunk[i].samples_per_chunk = heic_rb32(p + 4);
        t->sample_to_chunk[i].sample_desc_idx = heic_rb32(p + 8);
    }
    return 0;
}

static int seq_parse_stss(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t)
{
    uint32_t count, i;
    size_t needed;
    if (b->content_len < 8 || t->sync_samples) return -1;
    count = heic_rb32(b->content + 4);
    if (count > HEIC_SEQ_MAX_ENTRIES) return -1;
    needed = 8 + (size_t)count * 4;
    if (needed > b->content_len) return -1;
    if (!count) return 0;
    t->sync_samples = (uint32_t *)heic_zalloc(
        ctx, (size_t)count * sizeof(uint32_t));
    if (!t->sync_samples) return -1;
    t->n_sync_samples = count;
    for (i = 0; i < count; i++)
        t->sync_samples[i] = heic_rb32(b->content + 8 + (size_t)i * 4);
    return 0;
}

static int seq_parse_stts(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t)
{
    uint32_t count, i;
    if (b->content_len < 8 || b->content[0] != 0 || t->time_to_sample)
        return -1;
    count = heic_rb32(b->content + 4);
    if (!count || count > HEIC_SEQ_MAX_ENTRIES
        || (size_t)count > (b->content_len - 8) / 8)
        return -1;
    t->time_to_sample = (heic_seq_stts *)heic_zalloc(
        ctx, (size_t)count * sizeof(heic_seq_stts));
    if (!t->time_to_sample) return -1;
    t->n_time_to_sample = count;
    for (i = 0; i < count; i++) {
        const uint8_t *p = b->content + 8 + (size_t)i * 8;
        t->time_to_sample[i].count = heic_rb32(p);
        t->time_to_sample[i].delta = heic_rb32(p + 4);
        if (!t->time_to_sample[i].count || !t->time_to_sample[i].delta)
            return -1;
    }
    return 0;
}

static int seq_parse_ctts(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t)
{
    uint32_t count, i;
    int version;
    if (b->content_len < 8 || t->composition_offsets) return -1;
    version = b->content[0];
    if (version != 0 && version != 1) return -1;
    count = heic_rb32(b->content + 4);
    if (!count || count > HEIC_SEQ_MAX_ENTRIES
        || (size_t)count > (b->content_len - 8) / 8)
        return -1;
    t->composition_offsets = (heic_seq_ctts *)heic_zalloc(
        ctx, (size_t)count * sizeof(heic_seq_ctts));
    if (!t->composition_offsets) return -1;
    t->n_composition_offsets = count;
    for (i = 0; i < count; i++) {
        const uint8_t *p = b->content + 8 + (size_t)i * 8;
        t->composition_offsets[i].count = heic_rb32(p);
        t->composition_offsets[i].offset = version
            ? (int32_t)heic_rb32(p + 4)
            : (int64_t)heic_rb32(p + 4);
        if (!t->composition_offsets[i].count) return -1;
    }
    return 0;
}

static int seq_parse_stbl(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t, const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    box_iter_init(&it, b->content, b->content_len);
    while (box_iter_next(&it, &child)) {
        int rc = 0;
        if (heic_abort_check(ab)) return -1;
        if (child.type == HEIC_BOX_STSD)
            rc = seq_parse_stsd(ctx, &child, t, ab);
        else if (child.type == HEIC_BOX_STSZ)
            rc = seq_parse_stsz(ctx, &child, t);
        else if (child.type == HEIC_BOX_STCO)
            rc = seq_parse_chunk_offsets(ctx, &child, t, 0);
        else if (child.type == HEIC_BOX_CO64)
            rc = seq_parse_chunk_offsets(ctx, &child, t, 1);
        else if (child.type == HEIC_BOX_STSC)
            rc = seq_parse_stsc(ctx, &child, t);
        else if (child.type == HEIC_BOX_STSS)
            rc = seq_parse_stss(ctx, &child, t);
        else if (child.type == HEIC_BOX_STTS)
            rc = seq_parse_stts(ctx, &child, t);
        else if (child.type == HEIC_BOX_CTTS)
            rc = seq_parse_ctts(ctx, &child, t);
        if (rc != 0) return -1;
    }
    return 0;
}

static int seq_parse_minf(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t, const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    box_iter_init(&it, b->content, b->content_len);
    while (box_iter_next(&it, &child)) {
        if (heic_abort_check(ab)) return -1;
        if (child.type == HEIC_BOX_STBL
            && seq_parse_stbl(ctx, &child, t, ab) != 0)
            return -1;
    }
    return 0;
}

static int seq_parse_mdia(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t, const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    box_iter_init(&it, b->content, b->content_len);
    while (box_iter_next(&it, &child)) {
        if (heic_abort_check(ab)) return -1;
        if (child.type == HEIC_BOX_HDLR) {
            if (seq_parse_hdlr(&child, t) != 0) return -1;
        } else if (child.type == HEIC_BOX_MDHD) {
            if (seq_parse_mdhd(&child, t) != 0) return -1;
        } else if (child.type == HEIC_BOX_MINF) {
            if (seq_parse_minf(ctx, &child, t, ab) != 0) return -1;
        }
    }
    return 0;
}

static int seq_parse_trak(heic_ctx *ctx, const heic_box *b,
                          heic_seq_track *t, const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    memset(t, 0, sizeof(*t));
    box_iter_init(&it, b->content, b->content_len);
    while (box_iter_next(&it, &child)) {
        if (heic_abort_check(ab)) return -1;
        if (child.type == HEIC_BOX_TKHD) {
            if (seq_parse_tkhd(&child, t) != 0) return -1;
        } else if (child.type == HEIC_BOX_EDTS) {
            if (seq_parse_edts(ctx, &child, t) != 0) return -1;
        } else if (child.type == HEIC_BOX_MDIA) {
            if (seq_parse_mdia(ctx, &child, t, ab) != 0) return -1;
        }
    }
    return 0;
}

static uint32_t seq_sample_size(const heic_seq_track *t, uint32_t sample)
{
    if (!sample || sample > t->sample_count) return 0;
    if (t->uniform_sample_size) return t->uniform_sample_size;
    if (!t->sample_sizes) return 0;
    return t->sample_sizes[sample - 1];
}

static int seq_resolve_sample(const heic_seq_track *t, uint32_t sample,
                              uint64_t file_len, uint64_t *out_offset,
                              uint32_t *out_size, const heic_abort *ab)
{
    uint64_t current_sample = 1;
    uint32_t size, e;
    if (!sample || sample > t->sample_count || !t->chunk_offsets
        || !t->sample_to_chunk || !out_offset || !out_size)
        return -1;
    size = seq_sample_size(t, sample);
    if (!size) return -1;
    for (e = 0; e < t->n_sample_to_chunk; e++) {
        uint32_t first = t->sample_to_chunk[e].first_chunk;
        uint32_t per_chunk = t->sample_to_chunk[e].samples_per_chunk;
        uint32_t next = e + 1 < t->n_sample_to_chunk
            ? t->sample_to_chunk[e + 1].first_chunk
            : t->n_chunk_offsets + 1;
        uint32_t chunk;
        if (!first || !per_chunk || first > t->n_chunk_offsets
            || next < first)
            return -1;
        if (next > t->n_chunk_offsets + 1) next = t->n_chunk_offsets + 1;
        for (chunk = first; chunk < next; chunk++) {
            uint64_t chunk_end_sample;
            if ((chunk & 4095u) == 0 && heic_abort_check(ab)) return -1;
            chunk_end_sample = current_sample + per_chunk;
            if (chunk_end_sample < current_sample) return -1;
            if ((uint64_t)sample >= current_sample
                && (uint64_t)sample < chunk_end_sample) {
                uint64_t off = t->chunk_offsets[chunk - 1];
                uint64_t s;
                for (s = current_sample; s < sample; s++) {
                    uint32_t prev_size = seq_sample_size(t, (uint32_t)s);
                    if (!prev_size || off > UINT64_MAX - prev_size) return -1;
                    off += prev_size;
                }
                if (off > file_len || size > file_len - off) return -1;
                *out_offset = off;
                *out_size = size;
                return 0;
            }
            current_sample = chunk_end_sample;
        }
    }
    return -1;
}

static int seq_first_sample(const heic_seq_track *t, uint64_t file_len,
                            uint64_t *offset, uint32_t *size,
                            const heic_abort *ab)
{
    uint32_t sample = t->n_sync_samples ? t->sync_samples[0] : 1;
    return seq_resolve_sample(t, sample, file_len, offset, size, ab);
}

static uint64_t seq_rescale(uint64_t value, uint32_t from, uint32_t to)
{
    uint64_t q, r, scaled;
    if (!from || !to) return UINT64_MAX;
    q = value / from;
    r = value % from;
    if (q > UINT64_MAX / to) return UINT64_MAX;
    scaled = q * to;
    if (scaled > UINT64_MAX - (r * to) / from) return UINT64_MAX;
    return scaled + (r * to) / from;
}

static int seq_fill_sample_offsets(const heic_seq_track *t, uint64_t file_len,
                                   heic_sequence_sample *samples,
                                   const heic_abort *ab)
{
    uint32_t chunk, entry = 0, sample = 0, sync = 0;
    if (!t->chunk_offsets || !t->sample_to_chunk || !t->n_chunk_offsets
        || !t->n_sample_to_chunk
        || t->sample_to_chunk[0].first_chunk != 1)
        return -1;
    for (chunk = 1; chunk <= t->n_chunk_offsets && sample < t->sample_count;
         chunk++) {
        uint64_t off = t->chunk_offsets[chunk - 1];
        uint32_t j, per_chunk;
        if ((chunk & 4095u) == 0 && heic_abort_check(ab)) return -1;
        while (entry + 1 < t->n_sample_to_chunk
               && t->sample_to_chunk[entry + 1].first_chunk <= chunk)
            entry++;
        per_chunk = t->sample_to_chunk[entry].samples_per_chunk;
        if (!per_chunk) return -1;
        for (j = 0; j < per_chunk && sample < t->sample_count; j++, sample++) {
            uint32_t size = seq_sample_size(t, sample + 1);
            if (!size || off > file_len || size > file_len - off)
                return -1;
            samples[sample].offset = off;
            samples[sample].size = size;
            samples[sample].is_sync = (uint8_t)(
                !t->n_sync_samples
                || (sync < t->n_sync_samples
                    && t->sync_samples[sync] == sample + 1));
            if (samples[sample].is_sync && t->n_sync_samples) sync++;
            off += size;
        }
    }
    return sample == t->sample_count ? 0 : -1;
}

static int seq_fill_sample_times(const heic_seq_track *t,
                                 heic_sequence_sample *samples)
{
    uint64_t decode_time = 0;
    uint32_t sample = 0, i, j;
    if (!t->time_to_sample || !t->n_time_to_sample) return -1;
    for (i = 0; i < t->n_time_to_sample; i++) {
        for (j = 0; j < t->time_to_sample[i].count; j++) {
            uint32_t delta = t->time_to_sample[i].delta;
            if (sample >= t->sample_count || decode_time > INT64_MAX
                || decode_time > UINT64_MAX - delta)
                return -1;
            samples[sample].duration = delta;
            samples[sample].composition_time = (int64_t)decode_time;
            decode_time += delta;
            sample++;
        }
    }
    if (sample != t->sample_count) return -1;
    if (t->composition_offsets) {
        sample = 0;
        for (i = 0; i < t->n_composition_offsets; i++) {
            for (j = 0; j < t->composition_offsets[i].count; j++) {
                int64_t base, offset;
                if (sample >= t->sample_count) return -1;
                base = samples[sample].composition_time;
                offset = t->composition_offsets[i].offset;
                if ((offset > 0 && base > INT64_MAX - offset)
                    || (offset < 0 && base < INT64_MIN - offset))
                    return -1;
                samples[sample].composition_time = base + offset;
                sample++;
            }
        }
        if (sample != t->sample_count) return -1;
    }
    return 0;
}

static int seq_order_after(const heic_sequence_sample *samples,
                           uint32_t a, uint32_t b)
{
    return samples[a].composition_time > samples[b].composition_time
        || (samples[a].composition_time == samples[b].composition_time
            && a > b);
}

static void seq_heap_sift(const heic_sequence_sample *samples,
                          uint32_t *order, uint32_t root, uint32_t count)
{
    for (;;) {
        uint32_t child = root * 2 + 1, swap_at = root, tmp;
        if (child >= count) return;
        if (seq_order_after(samples, order[child], order[swap_at]))
            swap_at = child;
        if (child + 1 < count
            && seq_order_after(samples, order[child + 1], order[swap_at]))
            swap_at = child + 1;
        if (swap_at == root) return;
        tmp = order[root];
        order[root] = order[swap_at];
        order[swap_at] = tmp;
        root = swap_at;
    }
}

static void seq_sort_presentation(const heic_sequence_sample *samples,
                                  uint32_t *order, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++) order[i] = i;
    for (i = count / 2; i > 0; i--)
        seq_heap_sift(samples, order, i - 1, count);
    for (i = count; i > 1; i--) {
        uint32_t tmp = order[0];
        order[0] = order[i - 1];
        order[i - 1] = tmp;
        seq_heap_sift(samples, order, 0, i - 1);
    }
}

static int seq_add_frame(heic_sequence *seq, uint32_t capacity,
                         uint32_t sample, uint64_t time, uint64_t duration)
{
    uint32_t n = seq->frame_count;
    if (n >= capacity || duration > UINT32_MAX) return -1;
    seq->frame_samples[n] = sample;
    seq->frame_times[n] = time;
    seq->frame_durations[n] = (uint32_t)duration;
    seq->frame_count++;
    return 0;
}

static int seq_build_timeline(heic_ctx *ctx, heic_container *c,
                              const heic_seq_track *t,
                              uint32_t movie_timescale,
                              uint64_t movie_duration,
                              const heic_abort *ab)
{
    heic_sequence *seq = NULL;
    uint32_t *order = NULL;
    uint32_t capacity, i;
    int rc = -1;
    if (!t->sample_count || !t->media_timescale || !movie_timescale)
        return -1;
    for (i = 0; i < t->n_sync_samples; i++)
        if (!t->sync_samples[i] || t->sync_samples[i] > t->sample_count
            || (i && t->sync_samples[i] <= t->sync_samples[i - 1]))
            return -1;
    if (t->n_edits
        && (uint64_t)t->sample_count * t->n_edits > HEIC_SEQ_MAX_ENTRIES)
        return -1;
    capacity = t->n_edits ? t->sample_count * t->n_edits : t->sample_count;
    seq = (heic_sequence *)heic_zalloc(ctx, sizeof(heic_sequence));
    if (!seq) goto done;
    seq->samples = (heic_sequence_sample *)heic_zalloc(
        ctx, (size_t)t->sample_count * sizeof(heic_sequence_sample));
    seq->frame_samples = (uint32_t *)heic_zalloc(
        ctx, (size_t)capacity * sizeof(uint32_t));
    seq->frame_times = (uint64_t *)heic_zalloc(
        ctx, (size_t)capacity * sizeof(uint64_t));
    seq->frame_durations = (uint32_t *)heic_zalloc(
        ctx, (size_t)capacity * sizeof(uint32_t));
    order = (uint32_t *)heic_zalloc(
        ctx, (size_t)t->sample_count * sizeof(uint32_t));
    if (!seq->samples || !seq->frame_samples || !seq->frame_times
        || !seq->frame_durations || !order)
        goto done;
    seq->sample_count = t->sample_count;
    seq->timescale = movie_timescale;
    seq->duration = movie_duration;
    seq->repetition_count = t->n_edits ? 0 : 1;
    if (seq_fill_sample_offsets(t, c->len, seq->samples, ab) != 0
        || seq_fill_sample_times(t, seq->samples) != 0)
        goto done;
    seq_sort_presentation(seq->samples, order, t->sample_count);

    if (!t->n_edits) {
        int64_t first = seq->samples[order[0]].composition_time;
        for (i = 0; i < t->sample_count; i++) {
            uint32_t sample = order[i];
            int64_t pts = seq->samples[sample].composition_time;
            uint64_t time, duration;
            if (pts < first) goto done;
            time = seq_rescale((uint64_t)(pts - first),
                               t->media_timescale, movie_timescale);
            duration = seq_rescale(seq->samples[sample].duration,
                                   t->media_timescale, movie_timescale);
            if (time == UINT64_MAX || duration == UINT64_MAX
                || seq_add_frame(seq, capacity, sample, time, duration) != 0)
                goto done;
        }
    } else {
        uint64_t movie_cursor = 0;
        for (i = 0; i < t->n_edits; i++) {
            const heic_seq_edit *edit = &t->edits[i];
            uint32_t j;
            uint64_t media_span;
            if (edit->rate_integer != 1 || edit->rate_fraction != 0)
                goto done;
            if (edit->media_time >= 0) {
                media_span = seq_rescale(edit->segment_duration,
                                         movie_timescale,
                                         t->media_timescale);
                if (media_span == UINT64_MAX) goto done;
                for (j = 0; j < t->sample_count; j++) {
                    uint32_t sample = order[j];
                    int64_t pts = seq->samples[sample].composition_time;
                    uint64_t rel, time, duration;
                    if (pts < edit->media_time) continue;
                    rel = (uint64_t)(pts - edit->media_time);
                    if (rel >= media_span) continue;
                    time = seq_rescale(rel, t->media_timescale,
                                       movie_timescale);
                    duration = seq_rescale(seq->samples[sample].duration,
                                           t->media_timescale,
                                           movie_timescale);
                    if (time == UINT64_MAX || duration == UINT64_MAX
                        || movie_cursor > UINT64_MAX - time
                        || seq_add_frame(seq, capacity, sample,
                                         movie_cursor + time, duration) != 0)
                        goto done;
                }
            }
            if (movie_cursor > UINT64_MAX - edit->segment_duration)
                goto done;
            movie_cursor += edit->segment_duration;
        }
        if (t->edit_repeat && t->n_edits == 1
            && t->edits[0].media_time == 0
            && t->edits[0].rate_integer == 1
            && t->edits[0].rate_fraction == 0) {
            uint64_t segment = t->edits[0].segment_duration;
            if (movie_duration == UINT32_MAX || movie_duration == UINT64_MAX)
                seq->repetition_count = UINT32_MAX;
            else if (segment && movie_duration % segment == 0
                     && movie_duration / segment <= UINT32_MAX)
                seq->repetition_count =
                    (uint32_t)(movie_duration / segment);
        }
    }
    if (!seq->frame_count) goto done;
    c->sequence = seq;
    seq = NULL;
    rc = 0;
done:
    heic_free_buf(ctx, order);
    if (seq) {
        heic_free_buf(ctx, seq->samples);
        heic_free_buf(ctx, seq->frame_samples);
        heic_free_buf(ctx, seq->frame_times);
        heic_free_buf(ctx, seq->frame_durations);
        heic_free_buf(ctx, seq);
    }
    return rc;
}

static int seq_make_item(heic_ctx *ctx, heic_container *c,
                         heic_seq_track *t, int item_index, uint32_t item_id,
                         uint64_t offset, uint32_t size)
{
    heic_item_loc *loc = &c->item_locations[item_index];
    heic_ipma *assoc = &c->property_associations[item_index];
    heic_property *p;
    uint16_t props[3];
    int n_props = 0;

    c->item_infos[item_index].item_id = item_id;
    c->item_infos[item_index].item_type = HEIC_TYPE_HVC1;

    loc->item_id = item_id;
    loc->extents = (heic_extent *)heic_zalloc(ctx, sizeof(heic_extent));
    if (!loc->extents) return -1;
    loc->extents[0].offset = offset;
    loc->extents[0].length = size;
    loc->n_extents = 1;

    p = &c->properties[c->n_properties];
    p->kind = HEIC_PROP_ISPE;
    p->ispe.width = t->width;
    p->ispe.height = t->height;
    props[n_props++] = (uint16_t)++c->n_properties;

    p = &c->properties[c->n_properties];
    p->kind = HEIC_PROP_HVCC;
    p->hvcc = t->hvcc;
    memset(&t->hvcc, 0, sizeof(t->hvcc));
    t->has_hvcc = 0;
    props[n_props++] = (uint16_t)++c->n_properties;

    if (t->has_colr) {
        p = &c->properties[c->n_properties];
        p->kind = HEIC_PROP_COLR;
        p->colr = t->colr;
        memset(&t->colr, 0, sizeof(t->colr));
        t->has_colr = 0;
        props[n_props++] = (uint16_t)++c->n_properties;
    }
    return mini_make_assoc(ctx, assoc, item_id, props, n_props);
}

static int parse_moov(heic_ctx *ctx, const heic_box *moov,
                      heic_container *c, const heic_abort *ab)
{
    heic_seq_track tracks[HEIC_SEQ_MAX_TRACKS];
    heic_box_iter it;
    heic_box child;
    int n_tracks = 0, primary = -1, thumb = -1, i, rc = -1;
    uint64_t primary_off, thumb_off = 0;
    uint32_t primary_size, thumb_size = 0;
    uint32_t movie_timescale = 0;
    uint64_t movie_duration = 0;
    int n_items, n_props;

    memset(tracks, 0, sizeof(tracks));
    box_iter_init(&it, moov->content, moov->content_len);
    while (n_tracks < HEIC_SEQ_MAX_TRACKS && box_iter_next(&it, &child)) {
        if (heic_abort_check(ab)) goto done;
        if (child.type == HEIC_BOX_MVHD) {
            if (seq_parse_duration_header(&child, &movie_timescale,
                                          &movie_duration) != 0)
                goto done;
        } else if (child.type == HEIC_BOX_TRAK) {
            if (seq_parse_trak(ctx, &child, &tracks[n_tracks], ab) == 0)
                n_tracks++;
            else
                seq_free_track(ctx, &tracks[n_tracks]);
        }
    }
    for (i = 0; i < n_tracks; i++) {
        if (tracks[i].handler_type == HEIC_FCC('p', 'i', 'c', 't')
            && tracks[i].has_hvcc) {
            primary = i;
            break;
        }
    }
    if (primary < 0) {
        for (i = 0; i < n_tracks; i++) {
            if (tracks[i].handler_type == HEIC_FCC('v', 'i', 'd', 'e')
                && tracks[i].has_hvcc) {
                primary = i;
                break;
            }
        }
    }
    if (primary < 0) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "sequence has no HEVC image track");
        goto done;
    }
    if (!tracks[primary].width || !tracks[primary].height
        || tracks[primary].width > ctx->limits.max_width
        || tracks[primary].height > ctx->limits.max_height
        || (uint64_t)tracks[primary].width * tracks[primary].height
            > ctx->limits.max_pixels) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "invalid sequence dimensions");
        goto done;
    }
    if (seq_first_sample(&tracks[primary], c->len, &primary_off,
                         &primary_size, ab) != 0) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "cannot resolve sequence sync sample");
        goto done;
    }

    for (i = 0; i < n_tracks; i++) {
        if (i == primary || !tracks[i].has_hvcc
            || tracks[i].handler_type != HEIC_FCC('p', 'i', 'c', 't'))
            continue;
        if (tracks[i].width >= tracks[primary].width
            && tracks[i].height >= tracks[primary].height)
            continue;
        if (seq_first_sample(&tracks[i], c->len, &thumb_off,
                             &thumb_size, ab) == 0) {
            thumb = i;
            break;
        }
    }

    n_items = thumb >= 0 ? 2 : 1;
    n_props = 2 + tracks[primary].has_colr;
    if (thumb >= 0) n_props += 2 + tracks[thumb].has_colr;
    c->item_infos = (heic_item_info *)heic_zalloc(
        ctx, (size_t)n_items * sizeof(heic_item_info));
    c->item_locations = (heic_item_loc *)heic_zalloc(
        ctx, (size_t)n_items * sizeof(heic_item_loc));
    c->properties = (heic_property *)heic_zalloc(
        ctx, (size_t)n_props * sizeof(heic_property));
    c->property_associations = (heic_ipma *)heic_zalloc(
        ctx, (size_t)n_items * sizeof(heic_ipma));
    if (!c->item_infos || !c->item_locations || !c->properties
        || !c->property_associations)
        goto done;
    c->n_item_infos = n_items;
    c->n_item_locations = n_items;
    c->n_property_associations = n_items;
    c->primary_item_id = 1;
    if (seq_make_item(ctx, c, &tracks[primary], 0, 1,
                      primary_off, primary_size) != 0)
        goto done;
    if (!movie_timescale) movie_timescale = tracks[primary].media_timescale;
    if (!movie_duration)
        movie_duration = seq_rescale(tracks[primary].media_duration,
                                     tracks[primary].media_timescale,
                                     movie_timescale);
    (void)seq_build_timeline(ctx, c, &tracks[primary], movie_timescale,
                             movie_duration, ab);

    if (thumb >= 0) {
        c->item_references = (heic_iref *)heic_zalloc(ctx, sizeof(heic_iref));
        if (!c->item_references) goto done;
        c->n_item_references = 1;
        if (seq_make_item(ctx, c, &tracks[thumb], 1, 2,
                          thumb_off, thumb_size) != 0)
            goto done;
        c->item_references[0].ref_type = HEIC_REF_THMB;
        c->item_references[0].from_item_id = 2;
        c->item_references[0].to_item_ids =
            (uint32_t *)heic_zalloc(ctx, sizeof(uint32_t));
        if (!c->item_references[0].to_item_ids) goto done;
        c->item_references[0].to_item_ids[0] = 1;
        c->item_references[0].n_to = 1;
    }
    c->has_meta = 1;
    c->is_sequence = 1;
    rc = 0;

done:
    for (i = 0; i < n_tracks; i++) seq_free_track(ctx, &tracks[i]);
    return rc;
}

static int parse_meta(heic_ctx *ctx, const heic_box *meta, heic_container *c,
                      const heic_abort *ab)
{
    heic_box_iter it;
    heic_box child;
    if (meta->content_len < 4) return -1;
    box_iter_init(&it, meta->content + 4, meta->content_len - 4);
    while (box_iter_next(&it, &child)) {
        if (heic_abort_check(ab)) return -1;
        if (child.type == HEIC_BOX_PITM) {
            if (parse_pitm(ctx, &child, c) != 0) return -1;
        } else if (child.type == HEIC_BOX_ILOC) {
            if (parse_iloc(ctx, &child, c, ab) != 0) return -1;
        } else if (child.type == HEIC_BOX_IINF) {
            if (parse_iinf(ctx, &child, c, ab) != 0) return -1;
        } else if (child.type == HEIC_BOX_IPRP) {
            if (parse_iprp(ctx, &child, c, ab) != 0) return -1;
        } else if (child.type == HEIC_BOX_IREF) {
            if (parse_iref(ctx, &child, c, ab) != 0) return -1;
        } else if (child.type == HEIC_BOX_IDAT) {
            c->idat = child.content;
            c->idat_len = child.content_len;
        }
    }
    c->has_meta = 1;
    return 0;
}

int heic_container_parse(heic_ctx *ctx, const uint8_t *data, size_t len,
                         heic_container *out, const heic_abort *ab)
{
    heic_box_iter it;
    heic_box top;
    heic_box moov;
    int has_moov = 0;

    if (!ctx || !data || !out || len < 16) return -1;
    memset(out, 0, sizeof(*out));
    out->ctx = ctx;
    out->data = data;
    out->len = len;

    box_iter_init(&it, data, len);
    while (box_iter_next(&it, &top)) {
        if (heic_abort_check(ab)) {
            heic_container_free(out);
            return -1;
        }
        if (top.type == HEIC_BOX_FTYP) {
            if (parse_ftyp(ctx, &top, out) != 0) {
                heic_container_free(out);
                return -1;
            }
        } else if (top.type == HEIC_BOX_META) {
            /* HEIF still-image files have a single meta; ignore extras so a
             * fuzzer-crafted second meta cannot clobber half-built state. */
            if (out->has_meta) continue;
            if (parse_meta(ctx, &top, out, ab) != 0) {
                heic_container_free(out);
                return -1;
            }
        } else if (top.type == HEIC_BOX_MDAT) {
            out->mdat_offset = top.content_off;
            out->mdat_len = top.content_len;
        } else if (top.type == HEIC_BOX_MINI) {
            if (out->has_meta) continue;
            if (parse_mini(ctx, &top, out) != 0) {
                heic_container_free(out);
                return -1;
            }
        } else if (top.type == HEIC_BOX_MOOV && !has_moov) {
            /* Defer until all top-level boxes are seen: meta/mini takes
             * precedence when a file also carries an image sequence. */
            moov = top;
            has_moov = 1;
        }
    }
    if (!out->has_meta && has_moov) {
        if (parse_moov(ctx, &moov, out, ab) != 0) {
            heic_container_free(out);
            return -1;
        }
    }
    if (out->brand == 0 && out->n_compatible_brands == 0) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "missing ftyp");
        heic_container_free(out);
        return -1;
    }
    if (!out->has_meta) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "missing meta/mini/moov image");
        heic_container_free(out);
        return -1;
    }
    return 0;
}

int heic_container_get_item(const heic_container *c, uint32_t item_id, heic_item *out)
{
    int i, j;
    const heic_item_info *info = NULL;
    const heic_ipma *assoc = NULL;

    if (!c || !out) return -1;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < c->n_item_infos; i++)
        if (c->item_infos[i].item_id == item_id) {
            info = &c->item_infos[i];
            break;
        }
    if (!info) return -1;
    out->id = item_id;
    out->item_type = info->item_type;
    out->name = info->item_name ? info->item_name : "";
    out->content_type = info->content_type ? info->content_type : "";

    for (i = 0; i < c->n_property_associations; i++)
        if (c->property_associations[i].item_id == item_id) {
            assoc = &c->property_associations[i];
            break;
        }
    if (!assoc) return 0;

    for (j = 0; j < assoc->n_props; j++) {
        uint16_t idx = assoc->prop_indices[j];
        const heic_property *p;
        if (idx == 0 || idx > (uint16_t)c->n_properties) continue;
        p = &c->properties[idx - 1];
        switch (p->kind) {
        case HEIC_PROP_ISPE:
            out->has_dims = 1;
            out->width = p->ispe.width;
            out->height = p->ispe.height;
            break;
        case HEIC_PROP_HVCC:
            out->hvcc = &p->hvcc;
            break;
        case HEIC_PROP_AV1C:
            out->av1c = &p->av1c;
            break;
        case HEIC_PROP_COLR:
            out->colr = &p->colr;
            break;
        case HEIC_PROP_CLAP:
            out->clap = &p->clap;
            if (out->n_transforms < 8) {
                out->transforms[out->n_transforms].kind = HEIC_XFORM_CLAP;
                out->transforms[out->n_transforms].clap = p->clap;
                out->n_transforms++;
            }
            break;
        case HEIC_PROP_IROT:
            out->irot = &p->irot;
            if (out->n_transforms < 8) {
                out->transforms[out->n_transforms].kind = HEIC_XFORM_IROT;
                out->transforms[out->n_transforms].irot = p->irot;
                out->n_transforms++;
            }
            break;
        case HEIC_PROP_IMIR:
            out->imir = &p->imir;
            if (out->n_transforms < 8) {
                out->transforms[out->n_transforms].kind = HEIC_XFORM_IMIR;
                out->transforms[out->n_transforms].imir = p->imir;
                out->n_transforms++;
            }
            break;
        case HEIC_PROP_AUXC:
            out->auxc = &p->auxc;
            break;
        case HEIC_PROP_UNCC:
            out->uncc = &p->uncc;
            break;
        case HEIC_PROP_CMPC:
            out->cmpc = &p->cmpc;
            break;
        case HEIC_PROP_CMPD:
            out->cmpd = &p->cmpd;
            break;
        case HEIC_PROP_ICEF:
            out->icef = &p->icef;
            break;
        default:
            break;
        }
    }
    return 0;
}

int heic_container_item_data(const heic_container *c, uint32_t item_id,
                             const uint8_t **out_data, size_t *out_len, int *owned_out)
{
    const heic_item_loc *loc = NULL;
    const uint8_t *source;
    size_t source_len;
    int i;

    if (!c || !out_data || !out_len || !owned_out) return -1;
    *out_data = NULL;
    *out_len = 0;
    *owned_out = 0;

    for (i = 0; i < c->n_item_locations; i++)
        if (c->item_locations[i].item_id == item_id) {
            loc = &c->item_locations[i];
            break;
        }
    if (!loc || loc->n_extents == 0) return -1;

    if (loc->construction_method == 0) {
        source = c->data;
        source_len = c->len;
    } else if (loc->construction_method == 1) {
        if (!c->idat) return -1;
        source = c->idat;
        source_len = c->idat_len;
    } else
        return -1;

    if (loc->n_extents == 1) {
        uint64_t off64 = loc->base_offset + loc->extents[0].offset;
        uint64_t len64 = loc->extents[0].length;
        size_t off, length, end;
        if (off64 > SIZE_MAX || len64 > SIZE_MAX) return -1;
        off = (size_t)off64;
        length = (size_t)len64;
        if (off > source_len || length > source_len - off) return -1;
        end = off + length;
        if (end > source_len) return -1;
        *out_data = source + off;
        *out_len = length;
        *owned_out = 0;
        return 0;
    }

    /* multi-extent: concatenate */
    {
        uint64_t total = 0;
        size_t tlen;
        uint8_t *buf;
        size_t w = 0;
        for (i = 0; i < (int)loc->n_extents; i++) {
            if (total > UINT64_MAX - loc->extents[i].length) return -1;
            total += loc->extents[i].length;
        }
        if (total > source_len || total > SIZE_MAX) return -1;
        tlen = (size_t)total;
        buf = (uint8_t *)heic_zalloc(c->ctx, tlen ? tlen : 1);
        if (!buf) return -1;
        for (i = 0; i < (int)loc->n_extents; i++) {
            uint64_t off64 = loc->base_offset + loc->extents[i].offset;
            size_t off, length;
            if (off64 > SIZE_MAX) {
                heic_free_buf(c->ctx, buf);
                return -1;
            }
            off = (size_t)off64;
            length = (size_t)loc->extents[i].length;
            if (off > source_len || length > source_len - off) {
                heic_free_buf(c->ctx, buf);
                return -1;
            }
            memcpy(buf + w, source + off, length);
            w += length;
        }
        *out_data = buf;
        *out_len = tlen;
        *owned_out = 1;
        return 0;
    }
}

int heic_container_find_refs(const heic_container *c, uint32_t from_id,
                             heic_fourcc ref_type, uint32_t *out_ids, int max_out)
{
    int i, n = 0, j;
    if (!c) return 0;
    for (i = 0; i < c->n_item_references; i++) {
        const heic_iref *r = &c->item_references[i];
        if (r->from_item_id != from_id || r->ref_type != ref_type) continue;
        for (j = 0; j < r->n_to && n < max_out; j++)
            if (out_ids) out_ids[n++] = r->to_item_ids[j];
            else n++;
    }
    return n;
}

int heic_container_find_aux(const heic_container *c, uint32_t target_id,
                            const char *urn_prefix, uint32_t *out_ids, int max_out)
{
    int i, n = 0, j;
    size_t plen = urn_prefix ? strlen(urn_prefix) : 0;
    if (!c) return 0;
    for (i = 0; i < c->n_item_references; i++) {
        const heic_iref *r = &c->item_references[i];
        heic_item item;
        if (r->ref_type != HEIC_REF_AUXL) continue;
        for (j = 0; j < r->n_to; j++) {
            if (r->to_item_ids[j] != target_id) continue;
            if (heic_container_get_item(c, r->from_item_id, &item) != 0) continue;
            if (urn_prefix && item.auxc && item.auxc->aux_type &&
                strncmp(item.auxc->aux_type, urn_prefix, plen) != 0)
                continue;
            if (n < max_out && out_ids) out_ids[n] = r->from_item_id;
            n++;
        }
    }
    return n;
}

int heic_container_find_thumbs(const heic_container *c, uint32_t target_id,
                               uint32_t *out_ids, int max_out)
{
    int i, n = 0, j;
    if (!c) return 0;
    for (i = 0; i < c->n_item_references; i++) {
        const heic_iref *r = &c->item_references[i];
        if (r->ref_type != HEIC_REF_THMB) continue;
        for (j = 0; j < r->n_to; j++) {
            if (r->to_item_ids[j] != target_id) continue;
            if (n < max_out && out_ids) out_ids[n] = r->from_item_id;
            n++;
        }
    }
    return n;
}
