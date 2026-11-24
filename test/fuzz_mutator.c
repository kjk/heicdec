/* fuzz_mutator.c -- libFuzzer custom mutator that understands ISOBMFF.
 *
 * A byte-level mutation of a HEIF file usually changes a length somewhere
 * and the parser then rejects the file at the first box, long before the
 * interesting code. This mutator keeps the container intact: it walks the
 * box tree, mutates the payload of ONE box with libFuzzer's own mutator, and
 * then repairs what the size change broke -- the size field of that box and
 * of every box around it, and the absolute file offsets stored in iloc and
 * stco/co64 for data that moved.
 *
 * One call in four is left to the plain mutator so broken containers are
 * still explored, as is any input whose box tree cannot be walked.
 *
 * Linked into the libFuzzer builds of the targets that read whole files
 * (heic, container, diff). AFL++ has its own mutators and does not use it.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

size_t LLVMFuzzerMutate(uint8_t *data, size_t size, size_t max_size);
size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                               unsigned int seed);

#define MUT_MAX_DEPTH 8
#define MUT_MAX_LEAVES 256
#define MUT_MAX_TABLES 16

typedef struct {
    size_t hdr;     /* offset of the box header */
    size_t payload; /* offset of the payload */
    size_t end;     /* one past the box */
    int depth;      /* number of enclosing boxes */
    size_t parents[MUT_MAX_DEPTH]; /* header offsets of the enclosing boxes */
} mut_box;

typedef struct {
    size_t payload, end; /* payload extent of the table box */
    uint32_t type;       /* 'iloc', 'stco' or 'co64' */
} mut_table;

typedef struct {
    mut_box leaves[MUT_MAX_LEAVES];
    int n_leaves;
    mut_table tables[MUT_MAX_TABLES];
    int n_tables;
} mut_tree;

#define MUT_FCC(a, b, c, d)                                     \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) \
     | (uint32_t)(d))

static uint64_t mut_rd(const uint8_t *p, int n)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

static void mut_wr(uint8_t *p, int n, uint64_t v)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

/* Bytes between the box header and the child boxes, or -1 for a leaf. */
static int mut_container_skip(uint32_t type)
{
    switch (type) {
    case MUT_FCC('m', 'e', 't', 'a'):
        return 4; /* FullBox */
    case MUT_FCC('i', 'p', 'r', 'p'):
    case MUT_FCC('i', 'p', 'c', 'o'):
    case MUT_FCC('m', 'o', 'o', 'v'):
    case MUT_FCC('t', 'r', 'a', 'k'):
    case MUT_FCC('m', 'd', 'i', 'a'):
    case MUT_FCC('m', 'i', 'n', 'f'):
    case MUT_FCC('s', 't', 'b', 'l'):
    case MUT_FCC('d', 'i', 'n', 'f'):
    case MUT_FCC('e', 'd', 't', 's'):
        return 0;
    default:
        return -1;
    }
}

/* Collect the leaf boxes of [start, end). Returns 0 when the range is not a
   clean sequence of boxes. */
static int mut_walk(const uint8_t *d, size_t start, size_t end, int depth,
                    const size_t *parents, mut_tree *t)
{
    size_t pos = start;
    while (pos < end) {
        uint64_t size;
        size_t hdr_len = 8, box_end;
        uint32_t type;
        int skip;
        if (end - pos < 8) return 0;
        size = mut_rd(d + pos, 4);
        type = (uint32_t)mut_rd(d + pos + 4, 4);
        if (size == 1) {
            if (end - pos < 16) return 0;
            size = mut_rd(d + pos + 8, 8);
            hdr_len = 16;
        } else if (size == 0) {
            size = end - pos; /* to the end of the enclosing box */
        }
        if (size < hdr_len || size > end - pos) return 0;
        box_end = pos + (size_t)size;

        skip = mut_container_skip(type);
        if (skip >= 0 && depth < MUT_MAX_DEPTH
            && box_end - (pos + hdr_len) >= (size_t)skip) {
            size_t chain[MUT_MAX_DEPTH];
            if (depth) memcpy(chain, parents, (size_t)depth * sizeof(size_t));
            chain[depth] = pos;
            /* A container with a broken inside is mutated as one leaf. */
            {
                int before_leaves = t->n_leaves, before_tables = t->n_tables;
                if (mut_walk(d, pos + hdr_len + (size_t)skip, box_end,
                             depth + 1, chain, t)) {
                    pos = box_end;
                    continue;
                }
                t->n_leaves = before_leaves;
                t->n_tables = before_tables;
            }
        }
        if (t->n_leaves < MUT_MAX_LEAVES) {
            mut_box *b = &t->leaves[t->n_leaves++];
            b->hdr = pos;
            b->payload = pos + hdr_len;
            b->end = box_end;
            b->depth = depth;
            if (depth) memcpy(b->parents, parents, (size_t)depth * sizeof(size_t));
        }
        if ((type == MUT_FCC('i', 'l', 'o', 'c')
             || type == MUT_FCC('s', 't', 'c', 'o')
             || type == MUT_FCC('c', 'o', '6', '4'))
            && t->n_tables < MUT_MAX_TABLES) {
            mut_table *tb = &t->tables[t->n_tables++];
            tb->payload = pos + hdr_len;
            tb->end = box_end;
            tb->type = type;
        }
        pos = box_end;
    }
    return 1;
}

/* Add `delta` to the size field of the box whose header is at `hdr`. */
static void mut_fix_size(uint8_t *d, size_t hdr, int64_t delta)
{
    uint64_t size = mut_rd(d + hdr, 4);
    if (size == 0) return; /* "to the end": still true */
    if (size == 1)
        mut_wr(d + hdr + 8, 8, (uint64_t)((int64_t)mut_rd(d + hdr + 8, 8) + delta));
    else
        mut_wr(d + hdr, 4, (uint64_t)((int64_t)size + delta));
}

/* A file offset stored in `n` bytes at p: data at or after `from` moved. */
static void mut_shift(uint8_t *p, int n, size_t from, int64_t delta)
{
    uint64_t v;
    if (n != 4 && n != 8) return;
    v = mut_rd(p, n);
    if (v >= from) mut_wr(p, n, (uint64_t)((int64_t)v + delta));
}

static void mut_fix_iloc(uint8_t *d, size_t p, size_t end, size_t from,
                         int64_t delta)
{
    int version, off_sz, len_sz, base_sz, idx_sz = 0;
    uint32_t count, i;
    if (end - p < 8) return;
    version = d[p];
    off_sz = d[p + 4] >> 4;
    len_sz = d[p + 4] & 15;
    base_sz = d[p + 5] >> 4;
    if (version == 1 || version == 2) idx_sz = d[p + 5] & 15;
    p += 6;
    if (version < 2) {
        count = (uint32_t)mut_rd(d + p, 2);
        p += 2;
    } else {
        if (end - p < 4) return;
        count = (uint32_t)mut_rd(d + p, 4);
        p += 4;
    }
    for (i = 0; i < count; i++) {
        int method = 0;
        uint32_t extents, e;
        size_t need = (size_t)(version < 2 ? 2 : 4)
                      + (version >= 1 ? 2u : 0u) + 2u + (size_t)base_sz + 2u;
        uint64_t base;
        if (end - p < need) return;
        p += version < 2 ? 2 : 4;
        if (version >= 1) {
            method = d[p + 1] & 15;
            p += 2;
        }
        p += 2; /* data_reference_index */
        base = mut_rd(d + p, base_sz);
        /* Only construction_method 0 holds file offsets. With a non-zero
           base the extents are relative to it, so the base moves. */
        if (method == 0 && base != 0) mut_shift(d + p, base_sz, from, delta);
        p += (size_t)base_sz;
        extents = (uint32_t)mut_rd(d + p, 2);
        p += 2;
        for (e = 0; e < extents; e++) {
            if (end - p < (size_t)(idx_sz + off_sz + len_sz)) return;
            p += (size_t)idx_sz;
            if (method == 0 && base == 0) mut_shift(d + p, off_sz, from, delta);
            p += (size_t)(off_sz + len_sz);
        }
    }
}

static void mut_fix_chunk_offsets(uint8_t *d, size_t p, size_t end, int width,
                                  size_t from, int64_t delta)
{
    uint32_t count, i;
    if (end - p < 8) return;
    count = (uint32_t)mut_rd(d + p + 4, 4);
    p += 8;
    for (i = 0; i < count && end - p >= (size_t)width; i++, p += (size_t)width)
        mut_shift(d + p, width, from, delta);
}

static uint32_t mut_rand(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size, size_t max_size,
                               unsigned int seed)
{
    /* Static: libFuzzer calls the mutator from one thread per process, and
       this runs for every execution of targets that do thousands a second. */
    static mut_tree tree;
    static int disabled = -1;
    uint32_t rng = seed ? (uint32_t)seed : 0x9e3779b9u;
    const mut_box *box;
    size_t payload_len, room, tail, new_len, new_size;
    int64_t delta;
    int i;

    /* HEIC_FUZZ_NO_MUTATOR=1 (fuzz.ts -no-mutator): plain mutations only. */
    if (disabled < 0) disabled = getenv("HEIC_FUZZ_NO_MUTATOR") != NULL;
    if (disabled || (mut_rand(&rng) & 3) == 0 || size < 16 || size > max_size)
        return LLVMFuzzerMutate(data, size, max_size);
    tree.n_leaves = 0;
    tree.n_tables = 0;
    if (!mut_walk(data, 0, size, 0, NULL, &tree) || tree.n_leaves == 0)
        return LLVMFuzzerMutate(data, size, max_size);
    box = &tree.leaves[mut_rand(&rng) % (uint32_t)tree.n_leaves];
    payload_len = box->end - box->payload;
    room = max_size - size;
    if (payload_len + room == 0)
        return LLVMFuzzerMutate(data, size, max_size);

    /* In place: park what follows the box at the end of the buffer, let the
       payload grow or shrink into the gap, then pull the tail back. */
    tail = size - box->end;
    memmove(data + max_size - tail, data + box->end, tail);
    new_len = LLVMFuzzerMutate(data + box->payload, payload_len,
                               payload_len + room);
    memmove(data + box->payload + new_len, data + max_size - tail, tail);
    new_size = box->payload + new_len + tail;
    delta = (int64_t)new_len - (int64_t)payload_len;

    if (delta != 0) {
        mut_fix_size(data, box->hdr, delta);
        for (i = 0; i < box->depth; i++)
            mut_fix_size(data, box->parents[i], delta);
        /* Everything from the old end of the box moved by delta. Tables
           inside the mutated box are left as the mutator made them. */
        for (i = 0; i < tree.n_tables; i++) {
            const mut_table *tb = &tree.tables[i];
            size_t p = tb->payload, e = tb->end;
            if (p >= box->hdr && p < box->end) continue;
            if (p >= box->end) {
                p = (size_t)((int64_t)p + delta);
                e = (size_t)((int64_t)e + delta);
            }
            if (e > new_size || p > e) continue;
            if (tb->type == MUT_FCC('i', 'l', 'o', 'c'))
                mut_fix_iloc(data, p, e, box->end, delta);
            else
                mut_fix_chunk_offsets(
                    data, p, e, tb->type == MUT_FCC('s', 't', 'c', 'o') ? 4 : 8,
                    box->end, delta);
        }
    }
    return new_size;
}
