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
        }
        /* moov image sequences: later */
    }
    if (out->brand == 0 && out->n_compatible_brands == 0) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "missing ftyp");
        heic_container_free(out);
        return -1;
    }
    if (!out->has_meta) {
        heic_error(ctx, HEIC_SEVERITY_ERROR, "missing meta (image sequences not yet supported)");
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
