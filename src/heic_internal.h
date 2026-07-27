/* heic_internal.h -- single internal header for all decoder modules.
 *
 * Every src .c file includes only this header. Keep file-local (static)
 * symbols unique across translation units so the amalgamation builds.
 */
#ifndef HEIC_INTERNAL_H
#define HEIC_INTERNAL_H

#include "heic.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>

/* ---- helpers ---- */

#ifndef HEIC_MIN
#define HEIC_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef HEIC_MAX
#define HEIC_MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef HEIC_CLAMP
#define HEIC_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

#define HEIC_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* Default resource limits (imazen/heic Limits::server_defaults). */
#define HEIC_DEFAULT_MAX_WIDTH  16384u
#define HEIC_DEFAULT_MAX_HEIGHT 16384u
#define HEIC_DEFAULT_MAX_PIXELS ((uint64_t)256 * 1000 * 1000)
#define HEIC_DEFAULT_MAX_MEMORY ((size_t)1 * 1024 * 1024 * 1024)

/* Parser resource caps (adversarial input). */
#define HEIC_MAX_ITEMS            65536u
#define HEIC_MAX_PROPERTIES       65536u
#define HEIC_MAX_EXTENTS_PER_ITEM 1024u
#define HEIC_MAX_REFERENCES       65536u
#define HEIC_MAX_REFS_PER_ENTRY   4096u
#define HEIC_MAX_COMPAT_BRANDS    256
#define HEIC_MAX_STRING_LEN       4096
#define HEIC_MAX_NAL_UNIT_SIZE    (16 * 1024 * 1024)
#define HEIC_MAX_ICC_SIZE         (4 * 1024 * 1024)

/* HEVC UNINIT sentinel (must never leak to output). */
#define HEIC_UNINIT_SAMPLE 0xFFFFu

/* ---- fourcc ---- */

typedef uint32_t heic_fourcc;

#define HEIC_FCC(a, b, c, d) \
    ((heic_fourcc)(uint8_t)(a) | ((heic_fourcc)(uint8_t)(b) << 8) | \
     ((heic_fourcc)(uint8_t)(c) << 16) | ((heic_fourcc)(uint8_t)(d) << 24))

/* Box types (little-endian stored as BE fourcc in file; compare via heic_read_fcc). */
#define HEIC_BOX_FTYP HEIC_FCC('f', 't', 'y', 'p')
#define HEIC_BOX_META HEIC_FCC('m', 'e', 't', 'a')
#define HEIC_BOX_HDLR HEIC_FCC('h', 'd', 'l', 'r')
#define HEIC_BOX_PITM HEIC_FCC('p', 'i', 't', 'm')
#define HEIC_BOX_ILOC HEIC_FCC('i', 'l', 'o', 'c')
#define HEIC_BOX_IINF HEIC_FCC('i', 'i', 'n', 'f')
#define HEIC_BOX_INFE HEIC_FCC('i', 'n', 'f', 'e')
#define HEIC_BOX_IPRP HEIC_FCC('i', 'p', 'r', 'p')
#define HEIC_BOX_IPCO HEIC_FCC('i', 'p', 'c', 'o')
#define HEIC_BOX_IPMA HEIC_FCC('i', 'p', 'm', 'a')
#define HEIC_BOX_MDAT HEIC_FCC('m', 'd', 'a', 't')
#define HEIC_BOX_ISPE HEIC_FCC('i', 's', 'p', 'e')
#define HEIC_BOX_HVCC HEIC_FCC('h', 'v', 'c', 'C')
#define HEIC_BOX_HVCB HEIC_FCC('h', 'v', 'c', 'B')
#define HEIC_BOX_COLR HEIC_FCC('c', 'o', 'l', 'r')
#define HEIC_BOX_PIXI HEIC_FCC('p', 'i', 'x', 'i')
#define HEIC_BOX_IREF HEIC_FCC('i', 'r', 'e', 'f')
#define HEIC_BOX_AUXC HEIC_FCC('a', 'u', 'x', 'C')
#define HEIC_BOX_IDAT HEIC_FCC('i', 'd', 'a', 't')
#define HEIC_BOX_CLAP HEIC_FCC('c', 'l', 'a', 'p')
#define HEIC_BOX_IROT HEIC_FCC('i', 'r', 'o', 't')
#define HEIC_BOX_IMIR HEIC_FCC('i', 'm', 'i', 'r')
#define HEIC_BOX_AV1C HEIC_FCC('a', 'v', '1', 'C')
#define HEIC_BOX_UNCC HEIC_FCC('u', 'n', 'c', 'C')
#define HEIC_BOX_CMPC HEIC_FCC('c', 'm', 'p', 'C')
#define HEIC_BOX_CMPD HEIC_FCC('c', 'm', 'p', 'd')
#define HEIC_BOX_ICEF HEIC_FCC('i', 'c', 'e', 'f')
#define HEIC_BOX_CLLI HEIC_FCC('c', 'L', 'L', 'i')
#define HEIC_BOX_MDCV HEIC_FCC('m', 'D', 'C', 'v')
#define HEIC_BOX_MINI HEIC_FCC('m', 'i', 'n', 'i')
#define HEIC_BOX_MOOV HEIC_FCC('m', 'o', 'o', 'v')
#define HEIC_BOX_MVHD HEIC_FCC('m', 'v', 'h', 'd')
#define HEIC_BOX_TRAK HEIC_FCC('t', 'r', 'a', 'k')
#define HEIC_BOX_TKHD HEIC_FCC('t', 'k', 'h', 'd')
#define HEIC_BOX_EDTS HEIC_FCC('e', 'd', 't', 's')
#define HEIC_BOX_ELST HEIC_FCC('e', 'l', 's', 't')
#define HEIC_BOX_MDIA HEIC_FCC('m', 'd', 'i', 'a')
#define HEIC_BOX_MDHD HEIC_FCC('m', 'd', 'h', 'd')
#define HEIC_BOX_MINF HEIC_FCC('m', 'i', 'n', 'f')
#define HEIC_BOX_STBL HEIC_FCC('s', 't', 'b', 'l')
#define HEIC_BOX_STSD HEIC_FCC('s', 't', 's', 'd')
#define HEIC_BOX_STSZ HEIC_FCC('s', 't', 's', 'z')
#define HEIC_BOX_STCO HEIC_FCC('s', 't', 'c', 'o')
#define HEIC_BOX_CO64 HEIC_FCC('c', 'o', '6', '4')
#define HEIC_BOX_STSC HEIC_FCC('s', 't', 's', 'c')
#define HEIC_BOX_STSS HEIC_FCC('s', 't', 's', 's')
#define HEIC_BOX_STTS HEIC_FCC('s', 't', 't', 's')
#define HEIC_BOX_CTTS HEIC_FCC('c', 't', 't', 's')
#define HEIC_BOX_HVC1 HEIC_FCC('h', 'v', 'c', '1')
#define HEIC_BOX_HEV1 HEIC_FCC('h', 'e', 'v', '1')
#define HEIC_BOX_AV01 HEIC_FCC('a', 'v', '0', '1')
#define HEIC_BOX_TREF HEIC_FCC('t', 'r', 'e', 'f')
#define HEIC_BOX_AUXI HEIC_FCC('a', 'u', 'x', 'i')

/* Item type fourccs */
#define HEIC_TYPE_HVC1 HEIC_FCC('h', 'v', 'c', '1')
#define HEIC_TYPE_AV01 HEIC_FCC('a', 'v', '0', '1')
#define HEIC_TYPE_AVC1 HEIC_FCC('a', 'v', 'c', '1')
#define HEIC_TYPE_JPEG HEIC_FCC('j', 'p', 'e', 'g')
#define HEIC_TYPE_UNCI HEIC_FCC('u', 'n', 'c', 'i')
#define HEIC_TYPE_GRID HEIC_FCC('g', 'r', 'i', 'd')
#define HEIC_TYPE_IOVL HEIC_FCC('i', 'o', 'v', 'l')
#define HEIC_TYPE_IDEN HEIC_FCC('i', 'd', 'e', 'n')
#define HEIC_TYPE_TMAP HEIC_FCC('t', 'm', 'a', 'p')
#define HEIC_TYPE_EXIF HEIC_FCC('E', 'x', 'i', 'f')
#define HEIC_TYPE_MIME HEIC_FCC('m', 'i', 'm', 'e')

/* Reference types */
#define HEIC_REF_DIMG HEIC_FCC('d', 'i', 'm', 'g')
#define HEIC_REF_AUXL HEIC_FCC('a', 'u', 'x', 'l')
#define HEIC_REF_THMB HEIC_FCC('t', 'h', 'm', 'b')
#define HEIC_REF_CDSC HEIC_FCC('c', 'd', 's', 'c')
#define HEIC_REF_PRED HEIC_FCC('p', 'r', 'e', 'd')

/* ---- byte readers (big-endian ISOBMFF) ---- */

static inline uint16_t heic_rb16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t heic_rb32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint64_t heic_rb64(const uint8_t *p)
{
    return ((uint64_t)heic_rb32(p) << 32) | (uint64_t)heic_rb32(p + 4);
}
/* FourCC as file-order bytes packed into heic_fourcc (a in low byte). */
static inline heic_fourcc heic_read_fcc(const uint8_t *p)
{
    return HEIC_FCC(p[0], p[1], p[2], p[3]);
}


/* ---- context ---- */

struct heic_ctx {
    heic_alloc_cb alloc;
    heic_free_cb  free_cb;
    heic_error_cb error;
    void         *user;
    heic_limits   limits;
    /* Optional cached dav1d context (av1_dav1d.c); opaque to keep header free
     * of dav1d types. Closed in heic_ctx_free. */
    void         *dav1d_ctx;
};

void *heic_zalloc(heic_ctx *ctx, size_t size);
void *heic_realloc_buf(heic_ctx *ctx, void *p, size_t old_size, size_t new_size);
void  heic_free_buf(heic_ctx *ctx, void *p);
void  heic_error(heic_ctx *ctx, heic_severity sev, const char *fmt, ...);
int   heic_abort_check(const heic_abort *ab);

/* ---- HEIF container (heif.c) ---- */

typedef struct {
    int16_t x, y;
} heic_mv;

typedef struct {
    uint8_t pred_flag[2];
    int8_t ref_idx[2];
    heic_mv mv[2];
} heic_pb_motion;

typedef struct {
    uint64_t offset;
    uint64_t length;
} heic_extent;

typedef struct {
    uint32_t     item_id;
    uint8_t      construction_method; /* 0=file, 1=idat */
    uint64_t     base_offset;
    heic_extent *extents;
    uint32_t     n_extents;
} heic_item_loc;

typedef struct {
    uint32_t    item_id;
    heic_fourcc item_type;
    char       *item_name;     /* may be empty string */
    char       *content_type;  /* mime only */
    int         hidden;
} heic_item_info;

typedef struct {
    uint32_t width, height;
} heic_ispe;

typedef struct {
    uint8_t   config_version;
    uint8_t   general_profile_space;
    int       general_tier_flag;
    uint8_t   general_profile_idc;
    uint32_t  general_profile_compatibility_flags;
    uint64_t  general_constraint_indicator_flags;
    uint8_t   general_level_idc;
    uint8_t   chroma_format;
    uint8_t   bit_depth_luma_minus8;
    uint8_t   bit_depth_chroma_minus8;
    uint8_t   length_size_minus_one;
    /* Concatenated parameter-set NALs (each length-prefixed with 2 bytes BE). */
    uint8_t  *nal_blob;
    size_t    nal_blob_len;
    /* Or: array of individual NALs for convenience. */
    uint8_t **nal_units;
    size_t   *nal_unit_lens;
    int       n_nal_units;
} heic_hvcc;

typedef struct {
    uint8_t  seq_profile;
    uint8_t  seq_level_idx_0;
    int      high_bitdepth;
    int      twelve_bit;
    int      monochrome;
    int      chroma_subsampling_x;
    int      chroma_subsampling_y;
    uint8_t *config_obus;
    size_t   config_obus_len;
} heic_av1c;

typedef enum {
    HEIC_COLR_NONE = 0,
    HEIC_COLR_NCLX,
    HEIC_COLR_ICC
} heic_colr_kind;

typedef struct {
    heic_colr_kind kind;
    uint16_t color_primaries;
    uint16_t transfer_characteristics;
    uint16_t matrix_coefficients;
    int      full_range;
    uint8_t *icc;
    size_t   icc_len;
} heic_colr;

typedef struct {
    uint32_t width_n, width_d;
    uint32_t height_n, height_d;
    int32_t  horiz_off_n;
    uint32_t horiz_off_d;
    int32_t  vert_off_n;
    uint32_t vert_off_d;
} heic_clap;

typedef struct {
    uint16_t angle; /* 0, 90, 180, 270 CCW */
} heic_irot;

typedef struct {
    uint8_t axis; /* 0 = vertical axis (L-R), 1 = horizontal (T-B) */
} heic_imir;

typedef enum {
    HEIC_XFORM_CLAP = 1,
    HEIC_XFORM_IMIR,
    HEIC_XFORM_IROT
} heic_xform_kind;

typedef struct {
    heic_xform_kind kind;
    heic_clap clap;
    heic_imir imir;
    heic_irot irot;
} heic_xform;

typedef struct {
    char    *aux_type;
    uint8_t *subtype_data;
    size_t   subtype_len;
} heic_auxc;

/* ISO 23001-17 uncC component (FullBox body). */
typedef struct {
    uint16_t component_index; /* index into cmpd, or type if no cmpd */
    uint8_t  component_bit_depth_minus_one;
    uint8_t  component_format; /* 0 = unsigned integer */
    uint8_t  component_align_size;
} heic_uncc_comp;

typedef struct {
    uint32_t       profile;
    heic_uncc_comp *components;
    int            n_components;
    uint8_t        sampling_type;   /* 0=none, 1=422, 2=420, … */
    uint8_t        interleave_type; /* 0=comp, 1=pixel, 2=mixed, 3=row, 4=tile-comp */
    uint8_t        block_size;      /* 0 = no packing; else block bytes (1..8) */
    uint8_t        components_little_endian; /* flags 0x80 */
    uint8_t        block_pad_lsb;            /* 0x40 */
    uint8_t        block_little_endian;      /* 0x20 */
    uint8_t        block_reversed;           /* 0x10 */
    uint8_t        pad_unknown;              /* 0x08 */
    uint32_t       pixel_size;      /* ISO 23001-17: uint32 */
    uint32_t       row_align_size;
    uint32_t       tile_align_size;
    uint32_t       num_tile_cols_minus_one;
    uint32_t       num_tile_rows_minus_one;
} heic_uncc;

/* cmpC: generic compression fourcc (defl / zlib / brot). */
typedef struct {
    heic_fourcc compression_type;
    uint8_t     unit_type; /* 0=full item, 1=image, 2=tile, 3=row, 4=pixel */
} heic_cmpc;

/* cmpd: component type table (optional). Types: 0=mono,1=Y,2=Cb,3=Cr,4=R,5=G,6=B,7=A */
typedef struct {
    uint16_t *types;
    int       n_types;
} heic_cmpd;

/* icef: compressed unit offsets/sizes (ISO 23001-17). */
typedef struct {
    uint64_t offset;
    uint64_t size;
} heic_icef_unit;

typedef struct {
    heic_icef_unit *units;
    int             n_units;
} heic_icef;

typedef enum {
    HEIC_PROP_UNKNOWN = 0,
    HEIC_PROP_ISPE,
    HEIC_PROP_HVCC,
    HEIC_PROP_AV1C,
    HEIC_PROP_COLR,
    HEIC_PROP_CLAP,
    HEIC_PROP_IROT,
    HEIC_PROP_IMIR,
    HEIC_PROP_AUXC,
    HEIC_PROP_UNCC,
    HEIC_PROP_CMPC,
    HEIC_PROP_CMPD,
    HEIC_PROP_ICEF
} heic_prop_kind;

typedef struct {
    heic_prop_kind kind;
    heic_ispe ispe;
    heic_hvcc hvcc;
    heic_av1c av1c;
    heic_colr colr;
    heic_clap clap;
    heic_irot irot;
    heic_imir imir;
    heic_auxc auxc;
    heic_uncc uncc;
    heic_cmpc cmpc;
    heic_cmpd cmpd;
    heic_icef icef;
} heic_property;

typedef struct {
    uint32_t  item_id;
    uint16_t *prop_indices; /* 1-based indices into properties[] */
    uint8_t  *essential;    /* parallel flags */
    int       n_props;
} heic_ipma;

typedef struct {
    heic_fourcc  ref_type;
    uint32_t     from_item_id;
    uint32_t    *to_item_ids;
    int          n_to;
} heic_iref;

typedef struct {
    uint64_t offset;
    uint32_t size;
    uint32_t duration;
    int64_t  composition_time;
    uint8_t  is_sync;
} heic_sequence_sample;

typedef struct heic_sequence {
    uint32_t timescale;
    uint64_t duration;
    uint32_t repetition_count;
    uint32_t coded_item_id; /* synthetic item holding the moov track config */
    heic_sequence_sample *samples;
    uint32_t sample_count;
    uint32_t *frame_samples;
    uint64_t *frame_times;
    uint32_t *frame_durations;
    uint32_t frame_count;
    struct heic_sequence *alpha;
} heic_sequence;

typedef struct {
    heic_ctx          *ctx;
    const uint8_t     *data;
    size_t             len;
    heic_fourcc        brand;
    heic_fourcc        minor_brand;
    heic_fourcc       *compatible_brands;
    int                n_compatible_brands;
    uint32_t           primary_item_id;
    heic_item_loc     *item_locations;
    int                n_item_locations;
    heic_item_info    *item_infos;
    int                n_item_infos;
    heic_property     *properties;
    int                n_properties;
    heic_ipma         *property_associations;
    int                n_property_associations;
    heic_iref         *item_references;
    int                n_item_references;
    const uint8_t     *idat;
    size_t             idat_len;
    size_t             mdat_offset; /* content offset, or 0 */
    size_t             mdat_len;
    int                has_meta;
    int                is_sequence; /* moov image sequence is available */
    heic_sequence      *sequence;
} heic_container;

/* Resolved item view (stack-friendly; pointers into container). */
typedef struct {
    uint32_t           id;
    heic_fourcc        item_type;
    const char        *name;
    const char        *content_type;
    int                has_dims;
    uint32_t           width, height;
    const heic_hvcc   *hvcc;
    const heic_av1c   *av1c;
    const heic_colr   *colr;
    const heic_clap   *clap;
    const heic_irot   *irot;
    const heic_imir   *imir;
    const heic_auxc   *auxc;
    const heic_uncc   *uncc;
    const heic_cmpc   *cmpc;
    const heic_cmpd   *cmpd;
    const heic_icef   *icef;
    heic_xform         transforms[8];
    int                n_transforms;
} heic_item;

int  heic_container_parse(heic_ctx *ctx, const uint8_t *data, size_t len,
                          heic_container *out, const heic_abort *ab);
void heic_container_free(heic_container *c);
int  heic_container_get_item(const heic_container *c, uint32_t item_id,
                             heic_item *out);
/* Returns 0 on success. *out_data may point into container (borrowed) or
   be allocated (owned_out=1, free with heic_free_buf). */
int  heic_container_item_data(const heic_container *c, uint32_t item_id,
                              const uint8_t **out_data, size_t *out_len,
                              int *owned_out);
int  heic_container_find_refs(const heic_container *c, uint32_t from_id,
                              heic_fourcc ref_type, uint32_t *out_ids, int max_out);
int  heic_container_find_aux(const heic_container *c, uint32_t target_id,
                             const char *urn_prefix, uint32_t *out_ids, int max_out);
int  heic_container_find_thumbs(const heic_container *c, uint32_t target_id,
                                uint32_t *out_ids, int max_out);

/* ---- HEVC (hevc_*.c) ---- */

#define HEIC_MAX_REF_PICS 16
#define HEIC_MAX_ST_RPS 64
#define HEIC_MAX_LT_REF_PICS_SPS 32

typedef struct {
    int width, height;           /* coded */
    int crop_left, crop_right, crop_top, crop_bottom;
    int bit_depth;               /* luma (and alpha) */
    int chroma_bit_depth;        /* 0 for mono, otherwise chroma */
    int chroma_format;           /* 0=mono, 1=420, 2=422, 3=444 */
    int full_range;
    uint8_t matrix_coeffs;
    uint8_t color_primaries;
    uint8_t transfer_characteristics;
    uint16_t *y;
    uint16_t *cb;
    uint16_t *cr;
    /* Optional alpha plane (full-res, same dims as Y). Native bit depth. */
    uint16_t *a;
    int y_stride;                /* samples per row */
    int c_stride;
    int a_stride;                /* usually == y_stride when a present */
    int c_width, c_height;
    int poc;
    int poc_valid;
    heic_pb_motion *motion;
    uint8_t *motion_pred_mode;
    size_t motion_n;
    uint32_t motion_stride;
    uint32_t motion_min_pu;
    int ref_poc[2][HEIC_MAX_REF_PICS];
    uint8_t ref_long_term[2][HEIC_MAX_REF_PICS];
} heic_frame;

void heic_frame_free(heic_ctx *ctx, heic_frame *f);
int  heic_frame_alloc(heic_ctx *ctx, heic_frame *f, int w, int h,
                      int bit_depth, int chroma_format);

/* Decode HEVC still from hvcC + length-prefixed slice data. */
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

/* Decode AV1 still via dav1d (requires HEIC_HAVE_DAV1D + link dav1d). */
void heic_dav1d_ctx_close(heic_ctx *ctx); /* free cached dav1d on heic_ctx */
int heic_av1_decode(heic_ctx *ctx, const heic_av1c *cfg,
                    const uint8_t *data, size_t len,
                    heic_frame *out, const heic_abort *ab);
typedef struct heic_av1_sequence_state heic_av1_sequence_state;
heic_av1_sequence_state *heic_av1_sequence_new(heic_ctx *ctx);
void heic_av1_sequence_destroy(heic_av1_sequence_state *state);
int heic_av1_sequence_submit(heic_av1_sequence_state *state,
                             const heic_av1c *cfg,
                             const uint8_t *data, size_t len,
                             uint32_t sample_index, heic_frame *out,
                             uint32_t *out_sample, const heic_abort *ab);
int heic_av1_sequence_receive(heic_av1_sequence_state *state,
                              heic_frame *out, uint32_t *out_sample,
                              const heic_abort *ab);

/* Decode uncompressed HEIF (unci / ISO 23001-17). Interleave 0/1/3/4,
   multi-tile, 1..16-bit; optional zlib/deflate (HEIC_HAVE_ZLIB) and brotli
   (HEIC_HAVE_BROTLI); icef multi-unit when present. */
int heic_unci_decode(heic_ctx *ctx, const heic_uncc *uncc, const heic_cmpc *cmpc,
                     const heic_cmpd *cmpd, const heic_icef *icef,
                     const uint8_t *data, size_t len, uint32_t width,
                     uint32_t height, heic_frame *out, const heic_abort *ab);

/* YCbCr → interleaved RGB8 (applies crop). */
int heic_frame_to_rgb(heic_ctx *ctx, const heic_frame *f, heic_format format,
                      uint8_t *dst, int stride);

/* ---- document (document.c / decode.c) ---- */

struct heic_doc {
    heic_ctx       *ctx;
    const uint8_t  *data;
    size_t          len;
    heic_container  container;
    heic_kind       kind;
};

/* ---- NAL / bitstream helpers (hevc_bitstream.c) ---- */

typedef enum {
    HEIC_NAL_TRAIL_N = 0,
    HEIC_NAL_TRAIL_R = 1,
    HEIC_NAL_TSA_N = 2,
    HEIC_NAL_TSA_R = 3,
    HEIC_NAL_STSA_N = 4,
    HEIC_NAL_STSA_R = 5,
    HEIC_NAL_RADL_N = 6,
    HEIC_NAL_RADL_R = 7,
    HEIC_NAL_RASL_N = 8,
    HEIC_NAL_RASL_R = 9,
    HEIC_NAL_BLA_W_LP = 16,
    HEIC_NAL_BLA_W_RADL = 17,
    HEIC_NAL_BLA_N_LP = 18,
    HEIC_NAL_IDR_W_RADL = 19,
    HEIC_NAL_IDR_N_LP = 20,
    HEIC_NAL_CRA = 21,
    HEIC_NAL_VPS = 32,
    HEIC_NAL_SPS = 33,
    HEIC_NAL_PPS = 34,
    HEIC_NAL_AUD = 35,
    HEIC_NAL_EOS = 36,
    HEIC_NAL_EOB = 37,
    HEIC_NAL_FD = 38,
    HEIC_NAL_PREFIX_SEI = 39,
    HEIC_NAL_SUFFIX_SEI = 40,
    HEIC_NAL_UNKNOWN = 255
} heic_nal_type;

typedef struct {
    heic_nal_type type;
    uint8_t       nuh_layer_id;
    uint8_t       temporal_id;
    const uint8_t *payload;   /* RBSP (emulation prevention removed) */
    size_t         payload_len;
    uint8_t       *owned;     /* non-NULL if we allocated payload */
    /* Emulation-prevention byte positions in EBSP payload (after NAL header).
       Needed to map tile/WPP entry point offsets (EBSP) → RBSP seeks. */
    uint32_t     *ep_positions;
    int           n_ep_positions;
} heic_nal;

int  heic_nal_is_slice(heic_nal_type t);
int  heic_parse_length_prefixed(heic_ctx *ctx, const uint8_t *data, size_t len,
                                int length_size, heic_nal **out, int *out_n);
int  heic_parse_single_nal(heic_ctx *ctx, const uint8_t *data, size_t len,
                           heic_nal *out);
void heic_nal_free(heic_ctx *ctx, heic_nal *n);
void heic_nals_free(heic_ctx *ctx, heic_nal *nals, int n);

/* Bitstream reader over RBSP. */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         byte_pos;
    int            bit_pos; /* 0..7, next bit is MSB-first within byte */
    int            error;
} heic_bs;

void     heic_bs_init(heic_bs *bs, const uint8_t *data, size_t len);
int      heic_bs_bit(heic_bs *bs);
uint32_t heic_bs_bits(heic_bs *bs, int n);
uint32_t heic_bs_ue(heic_bs *bs);
int32_t  heic_bs_se(heic_bs *bs);
int      heic_bs_byte_aligned(const heic_bs *bs);
void     heic_bs_byte_align(heic_bs *bs);
size_t   heic_bs_bits_left(const heic_bs *bs);

/* ---- SPS/PPS (hevc_params.c) ---- */

typedef struct {
    /* Coefficients are stored in HEVC diagonal scan order. 16x16 and 32x32
       matrices hold their signaled 8x8 coefficients plus a separate DC. */
    uint8_t coef[4][6][64];
    uint8_t dc_coef[2][6];
} heic_scaling_list;

typedef struct {
    int32_t delta_poc_s0[HEIC_MAX_REF_PICS];
    int32_t delta_poc_s1[HEIC_MAX_REF_PICS];
    uint8_t used_by_curr_pic_s0[HEIC_MAX_REF_PICS];
    uint8_t used_by_curr_pic_s1[HEIC_MAX_REF_PICS];
    uint8_t num_negative_pics;
    uint8_t num_positive_pics;
} heic_st_rps;

typedef struct {
    uint8_t  sps_video_parameter_set_id;
    uint8_t  sps_max_sub_layers_minus1;
    int      sps_temporal_id_nesting_flag;
    /* profile_tier_level omitted fields that we only need partially */
    uint8_t  general_profile_idc;
    uint8_t  general_level_idc;
    uint8_t  chroma_format_idc;
    int      separate_colour_plane_flag;
    uint32_t pic_width_in_luma_samples;
    uint32_t pic_height_in_luma_samples;
    int      conformance_window_flag;
    uint32_t conf_win_left_offset;
    uint32_t conf_win_right_offset;
    uint32_t conf_win_top_offset;
    uint32_t conf_win_bottom_offset;
    uint8_t  bit_depth_luma_minus8;
    uint8_t  bit_depth_chroma_minus8;
    uint8_t  log2_max_pic_order_cnt_lsb_minus4;
    uint8_t  log2_min_luma_coding_block_size_minus3;
    uint8_t  log2_diff_max_min_luma_coding_block_size;
    uint8_t  log2_min_luma_transform_block_size_minus2;
    uint8_t  log2_diff_max_min_luma_transform_block_size;
    uint8_t  max_transform_hierarchy_depth_inter;
    uint8_t  max_transform_hierarchy_depth_intra;
    int      scaling_list_enabled_flag;
    int      sps_scaling_list_data_present_flag;
    heic_scaling_list scaling_list;
    int      amp_enabled_flag;
    int      sample_adaptive_offset_enabled_flag;
    int      pcm_enabled_flag;
    int      pcm_loop_filter_disabled_flag;
    uint8_t  pcm_sample_bit_depth_luma_minus1;
    uint8_t  pcm_sample_bit_depth_chroma_minus1;
    uint8_t  log2_min_pcm_luma_coding_block_size_minus3;
    uint8_t  log2_diff_max_min_pcm_luma_coding_block_size;
    uint8_t  num_short_term_ref_pic_sets;
    heic_st_rps short_term_rps[HEIC_MAX_ST_RPS];
    int      long_term_ref_pics_present_flag;
    uint8_t  num_long_term_ref_pics_sps;
    uint32_t lt_ref_pic_poc_lsb_sps[HEIC_MAX_LT_REF_PICS_SPS];
    uint8_t  used_by_curr_pic_lt_sps_flag[HEIC_MAX_LT_REF_PICS_SPS];
    int      sps_temporal_mvp_enabled_flag;
    int      strong_intra_smoothing_enabled_flag;
    int      vui_parameters_present_flag;
    int      video_signal_type_present_flag;
    int      video_full_range_flag;
    int      colour_description_present_flag;
    uint8_t  colour_primaries;
    uint8_t  transfer_characteristics;
    uint8_t  matrix_coeffs;
    int      transform_skip_rotation_enabled_flag;
    int      transform_skip_context_enabled_flag;
    int      implicit_rdpcm_enabled_flag;
    int      explicit_rdpcm_enabled_flag;
    int      extended_precision_processing_flag;
    int      intra_smoothing_disabled_flag;
    int      high_precision_offsets_enabled_flag;
    int      persistent_rice_adaptation_enabled_flag;
    int      cabac_bypass_alignment_enabled_flag;
    int      tiles_enabled; /* from PPS, but often needed with SPS dims */
    /* derived */
    uint8_t  log2_min_cb_size;
    uint8_t  log2_ctb_size;
    uint8_t  log2_min_tb_size;
    uint8_t  log2_max_tb_size;
    uint32_t ctb_width;
    uint32_t ctb_height;
    uint32_t pic_width_in_ctbs;
    uint32_t pic_height_in_ctbs;
    uint32_t pic_size_in_ctbs;
} heic_sps;

typedef struct {
    uint8_t  pps_pic_parameter_set_id;
    uint8_t  pps_seq_parameter_set_id;
    int      dependent_slice_segments_enabled_flag;
    int      output_flag_present_flag;
    uint8_t  num_extra_slice_header_bits;
    int      sign_data_hiding_enabled_flag;
    int      cabac_init_present_flag;
    uint8_t  num_ref_idx_l0_default_active_minus1;
    uint8_t  num_ref_idx_l1_default_active_minus1;
    int8_t   init_qp_minus26;
    int      constrained_intra_pred_flag;
    int      transform_skip_enabled_flag;
    int      cu_qp_delta_enabled_flag;
    uint8_t  diff_cu_qp_delta_depth;
    int8_t   pps_cb_qp_offset;
    int8_t   pps_cr_qp_offset;
    int      pps_slice_chroma_qp_offsets_present_flag;
    int      weighted_pred_flag;
    int      weighted_bipred_flag;
    int      transquant_bypass_enabled_flag;
    int      tiles_enabled_flag;
    int      entropy_coding_sync_enabled_flag;
    uint16_t num_tile_columns_minus1;
    uint16_t num_tile_rows_minus1;
    int      uniform_spacing_flag;
    uint16_t *column_width_minus1; /* optional */
    uint16_t *row_height_minus1;
    int      loop_filter_across_tiles_enabled_flag;
    int      pps_loop_filter_across_slices_enabled_flag;
    int      deblocking_filter_control_present_flag;
    int      deblocking_filter_override_enabled_flag;
    int      pps_deblocking_filter_disabled_flag;
    int8_t   pps_beta_offset_div2;
    int8_t   pps_tc_offset_div2;
    int      pps_scaling_list_data_present_flag;
    heic_scaling_list scaling_list;
    int      lists_modification_present_flag;
    uint8_t  log2_parallel_merge_level_minus2;
    int      slice_segment_header_extension_present_flag;
    int      pps_range_extension_flag;
    uint8_t  log2_max_transform_skip_block_size;
    int      cross_component_prediction_enabled_flag;
    int      chroma_qp_offset_list_enabled_flag;
    uint8_t  diff_cu_chroma_qp_offset_depth;
    uint8_t  chroma_qp_offset_list_len;
    int8_t   cb_qp_offset_list[6];
    int8_t   cr_qp_offset_list[6];
    uint8_t  log2_sao_offset_scale_luma;
    uint8_t  log2_sao_offset_scale_chroma;
} heic_pps;

int heic_parse_sps(heic_ctx *ctx, const uint8_t *rbsp, size_t len, heic_sps *out);
int heic_parse_pps(heic_ctx *ctx, const uint8_t *rbsp, size_t len, heic_pps *out);
int heic_parse_st_ref_pic_set(heic_bs *bs, int idx, int num_sets,
                              const heic_st_rps *sets, heic_st_rps *out);
void heic_pps_free(heic_ctx *ctx, heic_pps *pps);

/* ---- CABAC (hevc_cabac.c) ---- */

#define HEIC_NUM_CONTEXTS 170

/* Context indices (imazen/heic cabac::context) */
#define HEIC_CTX_SPLIT_CU_FLAG              0
#define HEIC_CTX_CU_TRANSQUANT_BYPASS_FLAG  3
#define HEIC_CTX_CU_SKIP_FLAG               4
#define HEIC_CTX_PRED_MODE_FLAG             8
#define HEIC_CTX_PART_MODE                  9
#define HEIC_CTX_PREV_INTRA_LUMA_PRED_FLAG  13
#define HEIC_CTX_INTRA_CHROMA_PRED_MODE     14
#define HEIC_CTX_INTER_PRED_IDC             15
#define HEIC_CTX_MERGE_FLAG                 20
#define HEIC_CTX_MERGE_IDX                  21
#define HEIC_CTX_MVP_LX_FLAG                22
#define HEIC_CTX_REF_IDX                    23
#define HEIC_CTX_ABS_MVD_GREATER0_FLAG      25
#define HEIC_CTX_RQT_ROOT_CBF               27
#define HEIC_CTX_SPLIT_TRANSFORM_FLAG       28
#define HEIC_CTX_CBF_LUMA                   31
#define HEIC_CTX_CBF_CBCR                   33
#define HEIC_CTX_TRANSFORM_SKIP_FLAG        38
#define HEIC_CTX_LAST_SIG_COEFF_X_PREFIX    40
#define HEIC_CTX_LAST_SIG_COEFF_Y_PREFIX    58
#define HEIC_CTX_CODED_SUB_BLOCK_FLAG       76
#define HEIC_CTX_SIG_COEFF_FLAG             80
#define HEIC_CTX_SIG_COEFF_FLAG_REXT        122
#define HEIC_CTX_COEFF_ABS_LEVEL_GREATER1   124
#define HEIC_CTX_COEFF_ABS_LEVEL_GREATER2   148
#define HEIC_CTX_SAO_MERGE_FLAG             154
#define HEIC_CTX_SAO_TYPE_IDX               155
#define HEIC_CTX_CU_QP_DELTA_ABS            156
#define HEIC_CTX_EXPLICIT_RDPCM_FLAG         158
#define HEIC_CTX_EXPLICIT_RDPCM_DIR          160

typedef struct {
    uint8_t state; /* 0..63 */
    uint8_t mps;   /* 0 or 1 */
} heic_ctx_model;

typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         byte_pos;
    uint32_t       range;
    uint32_t       value;
    int            bits_needed;
    uint32_t       overread_bytes;
    int            error;
} heic_cabac;

void heic_ctx_model_init(heic_ctx_model *m, uint8_t init_value, int slice_qp);
void heic_cabac_init_contexts(heic_ctx_model *ctx, int slice_type, int cabac_init_flag,
                              int slice_qp);
int  heic_cabac_new(heic_cabac *c, const uint8_t *data, size_t len);
void heic_cabac_reinit(heic_cabac *c);
void heic_cabac_seek(heic_cabac *c, size_t byte_pos);
int  heic_cabac_overread(const heic_cabac *c);
int  heic_cabac_decode_bin(heic_cabac *c, heic_ctx_model *ctx);
int  heic_cabac_decode_bypass(heic_cabac *c);
uint32_t heic_cabac_decode_bypass_bits(heic_cabac *c, int n);
int  heic_cabac_decode_terminate(heic_cabac *c);
uint32_t heic_cabac_decode_egk(heic_cabac *c, int k);

/* ---- slice header (hevc_slice.c) ---- */

#define HEIC_SLICE_B 0
#define HEIC_SLICE_P 1
#define HEIC_SLICE_I 2

typedef struct {
    int      first_slice_segment_in_pic_flag;
    int      no_output_of_prior_pics_flag;
    uint8_t  pps_id;
    int      dependent_slice_segment_flag;
    uint32_t slice_segment_address;
    uint32_t slice_address; /* address of the owning independent slice */
    int      slice_type; /* HEIC_SLICE_* */
    int      pic_output_flag;
    uint8_t  colour_plane_id;
    uint32_t slice_pic_order_cnt_lsb;
    uint8_t  short_term_ref_pic_set_idx;
    int      has_inline_short_term_rps;
    heic_st_rps inline_short_term_rps;
    uint8_t  num_long_term_sps;
    uint8_t  num_long_term_pics;
    uint8_t  lt_idx_sps[HEIC_MAX_REF_PICS];
    uint32_t poc_lsb_lt[HEIC_MAX_REF_PICS];
    uint8_t  used_by_curr_pic_lt_flag[HEIC_MAX_REF_PICS];
    uint8_t  delta_poc_msb_present_flag[HEIC_MAX_REF_PICS];
    uint32_t delta_poc_msb_cycle_lt[HEIC_MAX_REF_PICS];
    int      slice_temporal_mvp_enabled_flag;
    int      slice_sao_luma_flag;
    int      slice_sao_chroma_flag;
    uint8_t  num_ref_idx_l0_active;
    uint8_t  num_ref_idx_l1_active;
    int      ref_pic_list_modification_flag_l0;
    int      ref_pic_list_modification_flag_l1;
    uint8_t  list_entry_l0[HEIC_MAX_REF_PICS];
    uint8_t  list_entry_l1[HEIC_MAX_REF_PICS];
    int      has_pred_weight_table;
    uint8_t  luma_log2_weight_denom;
    uint8_t  chroma_log2_weight_denom;
    int16_t  luma_weight[2][HEIC_MAX_REF_PICS];
    int16_t  luma_offset[2][HEIC_MAX_REF_PICS];
    int16_t  chroma_weight[2][HEIC_MAX_REF_PICS][2];
    int16_t  chroma_offset[2][HEIC_MAX_REF_PICS][2];
    int      mvd_l1_zero_flag;
    int      collocated_from_l0_flag;
    uint8_t  collocated_ref_idx;
    uint8_t  max_num_merge_cand;
    int8_t   slice_qp_delta;
    int8_t   slice_cb_qp_offset;
    int8_t   slice_cr_qp_offset;
    int      deblocking_filter_override_flag;
    int      slice_deblocking_filter_disabled_flag;
    int8_t   slice_beta_offset_div2;
    int8_t   slice_tc_offset_div2;
    int      slice_loop_filter_across_slices_enabled_flag;
    uint32_t  num_entry_point_offsets;
    uint32_t *entry_point_offsets; /* owned by caller free via heic_slice_header_free */
    int       cabac_init_flag;
    int       slice_qp_y;
    size_t    data_offset; /* byte offset of slice_data in RBSP */
} heic_slice_header;

void heic_slice_header_free(heic_ctx *ctx, heic_slice_header *sh);

int heic_parse_slice_header(heic_ctx *ctx, const heic_nal *nal,
                            const heic_sps *sps, const heic_pps *pps,
                            const heic_slice_header *independent,
                            heic_slice_header *out);

/* ---- residual (hevc_residual.c) ---- */

#define HEIC_MAX_COEFF 1024

enum {
    HEIC_SCAN_DIAG = 0,
    HEIC_SCAN_HORIZ = 1,
    HEIC_SCAN_VERT = 2
};

typedef struct {
    int16_t coeffs[HEIC_MAX_COEFF];
    uint8_t log2_size;
    uint16_t num_nonzero;
} heic_coeff_buf;

int heic_get_scan_order(uint8_t log2_size, uint8_t intra_mode, uint8_t c_idx,
                        int chroma_444);
/* Returns 0 on success. rdpcm_mode is 0=off, 1=horizontal, 2=vertical. */
int heic_decode_residual(heic_cabac *cabac, heic_ctx_model *ctx,
                         uint8_t log2_size, uint8_t c_idx, int scan_order,
                         int sign_data_hiding, int cu_transquant_bypass,
                         int transform_skip_enabled, uint8_t max_transform_skip_log2,
                         int transform_skip_context_enabled,
                         int implicit_rdpcm_enabled, int explicit_rdpcm_enabled,
                         int persistent_rice_adaptation_enabled,
                         uint8_t stat_coeff[4],
                         int pred_mode_intra, uint8_t intra_mode,
                         heic_coeff_buf *out, int *transform_skip, int *rdpcm_mode);

/* ---- transform / dequant (hevc_transform.c) ---- */

void heic_idst4(const int16_t *coeffs /*16*/, int16_t *out /*16*/, int bit_depth);
void heic_idct4(const int16_t *coeffs, int16_t *out, int bit_depth);
void heic_idct8(const int16_t *coeffs /*64*/, int16_t *out, int bit_depth);
void heic_idct16(const int16_t *coeffs /*256*/, int16_t *out, int bit_depth);
void heic_idct32(const int16_t *coeffs /*1024*/, int16_t *out, int bit_depth);
/* Thread-local 32×32 int32 scratch for separable IDCT (not re-entrant / recursive). */
int32_t *heic_idct_scratch_buf(void);
void heic_dequantize(int16_t *coeffs, int n, int qp, int bit_depth,
                     uint8_t log2_tr_size);
void heic_dequantize_scaled(int16_t *coeffs, int n, int qp, int bit_depth,
                            uint8_t log2_tr_size, const heic_scaling_list *list,
                            uint8_t matrix_id);
void heic_inverse_transform(const int16_t *coeffs, int16_t *output, int size,
                            int bit_depth, int is_intra_4x4_luma);
void heic_add_residual(uint16_t *plane, int stride, int x0, int y0,
                       const int16_t *residual, int size, int max_val);

/* ---- SIMD (hevc_simd.c); return 1 if handled, 0 = use scalar ---- */
void heic_simd_init(void);
int  heic_simd_enabled(void);
int  heic_simd_idct8(const int16_t *coeffs, int16_t *out, int bit_depth);
int  heic_simd_idct16(const int16_t *coeffs, int16_t *out, int bit_depth);
int  heic_simd_idct32(const int16_t *coeffs, int16_t *out, int bit_depth);
int  heic_simd_add_residual(uint16_t *plane, int stride, int x0, int y0,
                            const int16_t *residual, int size, int max_val);
/* 8-bit 4:4:4 row: yp/cbp/crp point at x0; write w RGB pixels to row. */
int  heic_simd_ycc_444_row(const uint16_t *yp, const uint16_t *cbp, const uint16_t *crp,
                           uint8_t *row, int w, int full,
                           const int32_t yv[256], const int32_t cr_r[256],
                           const int32_t cb_g[256], const int32_t cr_g[256],
                           const int32_t cb_b[256]);
/* 8-bit 4:2:0 row: cbp/crp point at floor(x0/2); x_phase is x0&1. */
int  heic_simd_ycc_420_row(const uint16_t *yp, const uint16_t *cbp, const uint16_t *crp,
                           uint8_t *row, int w, int x_phase, int full,
                           const int32_t yv[256], const int32_t cr_r[256],
                           const int32_t cb_g[256], const int32_t cr_g[256],
                           const int32_t cb_b[256]);
/* Chroma edge: 4 samples along edge (vert: stride between samples; horiz: 1). */
int  heic_simd_chroma_edge4(uint16_t *plane, int stride, size_t base_q0, int across,
                            int tc, int max_val, int along_is_stride);
/* Luma deblock apply for 4 samples along edge after strong/d_ep/d_eq decided. */
int  heic_simd_luma_filter4(uint16_t *plane, size_t base_p, size_t base_q,
                            size_t step_along, size_t step_across, int strong,
                            int d_ep, int d_eq, int tc, int max_val);
/* Dequant common path (combined fits int32, shift>=0). */
int  heic_simd_dequant(int16_t *coeffs, int n, int32_t combined, int shift);
/* Angular row: pred[i] = ((a*ref[i] + b*ref[i+1] + 16)>>5) clipped, i=0..n-1. */
int  heic_simd_intra_ang_row(uint16_t *dst, const int32_t *ref, int n, int a, int b,
                             int max_val);
/* Copy n u16 plane samples → int32 border; set avail[i]=1. */
int  heic_simd_u16_to_i32_avail(const uint16_t *src, int32_t *border, int *avail, int n);
/* Top extension: copy if != UNINIT; returns how many were valid. */
int  heic_simd_border_top_ext(const uint16_t *src, int32_t *border, int *avail, int n);
/* Angular mode&lt;18: per-pixel angle index along a row (row_base + f(px)). */
int  heic_simd_intra_ang_row_var(uint16_t *dst, const int32_t *ref, int n, int row_base,
                                 int32_t angle, int max_val);
/* SAO band interior row: x0..x1 on one scanline. */
int  heic_simd_sao_band_row(uint16_t *row, int x0, int x1, int band_shift,
                            const int16_t band_table[32], int max_val);
/* SAO edge class 0 (horizontal neighbors) interior row. */
int  heic_simd_sao_edge_h_row(const uint16_t *srow, uint16_t *drow, int x0, int x1,
                              const int offset_table[5], int max_val);
/* SAO edge class 1 (vertical neighbors) interior row. */
int  heic_simd_sao_edge_v_row(const uint16_t *src, uint16_t *dst, int stride, int y,
                              int x0, int x1, const int offset_table[5], int max_val);

/* ---- intra prediction (hevc_intra.c) ---- */

void heic_fill_mpm(uint8_t cand_a, uint8_t cand_b, uint8_t mpm[3]);
/* mode: 0=Planar, 1=DC, 2-34=angular. c_idx: 0=Y,1=Cb,2=Cr. */
int heic_predict_intra(heic_frame *frame, uint32_t x, uint32_t y,
                       uint8_t log2_size, uint8_t mode, uint8_t c_idx,
                       int strong_intra_smoothing, uint32_t slice_address,
                       uint32_t pic_width_in_ctbs, uint32_t ctb_size);

/* ---- SAO filter (hevc_sao.c) ---- */

typedef struct {
    uint8_t sao_type_idx[3];      /* 0=off, 1=band, 2=edge per Y/Cb/Cr */
    int16_t sao_offset_val[3][4];
    uint8_t sao_band_position[3];
    uint8_t sao_eo_class[3];
} heic_sao_info;

void heic_apply_sao(heic_ctx *ctx, heic_frame *frame, const heic_sao_info *map,
                    uint32_t width_ctbs, uint32_t height_ctbs, uint32_t ctb_size,
                    const uint8_t *pcm_map, uint32_t pcm_stride);

/* ---- deblock filter (hevc_deblock.c) ---- */

void heic_mark_tu_boundary(uint8_t *flags, uint32_t deblock_stride, uint32_t map_n,
                           uint32_t x, uint32_t y, uint32_t size);
void heic_store_deblock_qp(int8_t *qp_map, uint32_t deblock_stride, uint32_t map_n,
                           uint32_t x, uint32_t y, uint32_t size, int8_t qp);
void heic_mark_pb_boundary(uint8_t *flags, uint32_t deblock_stride, uint32_t map_n,
                           uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                           int vertical);
/* I-slice deblock: bS=2 on all marked edges. Offsets are * 2 (div2 * 2). */
void heic_apply_deblock(heic_frame *frame, const uint8_t *flags, const int8_t *qp_map,
                        uint32_t deblock_stride, int beta_offset, int tc_offset,
                        int cb_qp_offset, int cr_qp_offset,
                        const uint8_t *pred_mode, const heic_pb_motion *mv_info,
                        uint32_t pu_stride, uint32_t min_pu,
                        const uint8_t *cbf_map,
                        const int ref_poc[2][HEIC_MAX_REF_PICS],
                        const uint8_t *pcm_map);

/* ---- inter prediction / CTU decode (hevc_inter.c, hevc_ctu.c) ---- */

int heic_mc_luma(const heic_frame *ref, heic_frame *dst, heic_mv mv,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 int32_t *scratch, size_t scratch_n);
int heic_mc_chroma(const heic_frame *ref, heic_frame *dst, heic_mv mv,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   int32_t *scratch, size_t scratch_n);
int heic_mc_luma_internal(const heic_frame *ref, heic_mv mv,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          int16_t *out, uint32_t out_stride,
                          int32_t *scratch, size_t scratch_n);
int heic_mc_chroma_internal(const heic_frame *ref, heic_mv mv,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            int16_t *out_cb, int16_t *out_cr,
                            uint32_t out_stride,
                            int32_t *scratch, size_t scratch_n);

typedef struct heic_hevc_picture heic_hevc_picture;

heic_hevc_picture *heic_hevc_picture_new(
    heic_ctx *ctx, const heic_sps *sps, const heic_pps *pps,
    const heic_slice_header *sh,
    const heic_frame *const *l0, int n_l0,
    const heic_frame *const *l1, int n_l1, heic_frame *out);
int heic_hevc_picture_decode_segment(
    heic_hevc_picture *picture, const heic_slice_header *sh,
    const uint8_t *data, size_t len,
    const uint32_t *ep_positions, int n_ep,
    const heic_frame *const *l0, int n_l0,
    const heic_frame *const *l1, int n_l1,
    const heic_abort *ab);
int heic_hevc_picture_finish(heic_hevc_picture *picture);
void heic_hevc_picture_destroy(heic_hevc_picture *picture);

/* ---- decode orchestration (decode.c) ---- */
int heic_decode_primary(heic_doc *doc, heic_format format,
                        heic_image **out_img, uint8_t *into, size_t into_size,
                        int into_stride, const heic_abort *ab);

#endif /* HEIC_INTERNAL_H */
