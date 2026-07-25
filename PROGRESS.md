# PROGRESS

## Design decisions (locked)

- **HEVC**: pure-C port of imazen/heic (still images / I-frames first).
- **AV1**: use **dav1d C** (videolan). Do **not** port rav1d-safe.
- **Oracle / tests**: libheif + dav1d; corpus from `deps/heic/testdata`.
- **Style**: djvudec — single internal header, bun scripts, amalgamation.
- **SumatraPDF**: size probe, RGB decode, EXIF extract from memory buffer.

## Done

- [x] `src/heic.h` public API
- [x] `src/heic_internal.h` module declarations
- [x] `src/core.c` ctx/alloc/frame
- [x] deps: imazen/heic, dav1d, libheif (via get-deps) — **no rav1d-safe**
- [x] AGENTS.md / README / LICENSE / .gitignore
- [x] `src/heif.c` — ISOBMFF/HEIF parse (ftyp/meta/iloc/iinf/iprp/iref/idat/mdat, properties)
- [x] `src/document.c` — open, info, EXIF/XMP/ICC
- [x] `src/decode.c` — grid, iden/tmap, iovl, irot/imir transforms
- [x] `src/color.c` — YCbCr→RGB
- [x] HEVC full I-slice path (bitstream, params, cabac, slice, residual, transform, intra, CTU)
- [x] WPP + tiles with entry-point CABAC reinit
- [x] Pixels: `single.heic`, `grid.heic`, `example.heic` 1280×854
- [x] features corpus: 25/25 -info ok; 23/25 full decode (2 intentional: bad iovl version, huge canvas)
- [x] Alpha plane from `auxl` (HEVC auxid:1 + CICP alpha URN) → `frame->a` / RGBA A channel
- [x] `av1_dav1d.c` real dav1d path (`HEIC_HAVE_DAV1D`); `bun cmd/build.ts -dav1d`
- [x] Static dav1d build under `out/dav1d_build` (meson; `enable_asm` when nasm present)
- [x] Smoke: `example.avif` 800×533; `alpha.heic` 64×64 RGBA
- [x] `cmd/{get-deps,build,tests,build-dist,build-wasm,verify-wasm,corpus,gen-cabac-init}.ts` + harness
- [x] WebAssembly drop: `wasm/heic.js` + `wasm/index.html` (HEVC/unci; no dav1d)
- [x] Nokia `heif_conformance` in get-deps + corpus (`tests.ts` / `bench.ts` -all)

- [x] SAO filter apply (`hevc_sao.c`) after I-slice CTU loop
- [x] Deblocking filter (`hevc_deblock.c`) I-slice bS=2; TU edges marked in CTU path; order deblock→SAO
- [x] Clean aperture (`clap`) crop + display dims; grid tile layout uses post-clap size
- [x] Intra PART_NxN PB edge marks for deblock
- [x] Pixel oracle path: `heic_test -verify` vs native libheif (also `cmd/oracle_pixels.ts` optional)
- [x] Known good vs libheif: lossless exact; lossy single-family mse≈100
- [x] Native libheif oracle stack: `bun cmd/build.ts ref` → libde265 + dav1d + libheif static
- [x] `cmd/bench.ts` + `heic_test -bench` (djvudec-style open/decode/close vs libheif)

- [x] irot/imir: fix imir axis (0=TB, 1=LR) and 90° crop remap (L←B,R←T,T←L,B←R)
- [x] YCbCr→RGB full/limited coeffs match imazen/libheif (features mse=0 vs libheif)

## Perf (2026-07)

- MSVC harness builds emit `out/msvc/heic_test_msvc.pdb` (`-Zi` + link `-DEBUG`) for samply.
- `heic_test -profile-heic N` / `-profile-libheif N` loop decode under samply section marks.
- Profile-guided wins on HEVC stills (samply `-print-agent` on `nokia_444` 2048×2048):
  - IDCT: pure-DC fast path + skip zero columns (idct8/16/32)
  - Color: specialized 8-bit full/limited 4:4:4 and 4:2:0 RGB loops (no per-pixel UNINIT)
  - Intra: direct plane writes (no bounds checks); DC row fill
  - Frame alloc: `memset(0xFF)` for UNINIT instead of scalar loops
  - AV1: cache `Dav1dContext` on `heic_ctx`; wider 8→16 plane copy
- Color: per-frame 8-bit LUTs (limited/full) — table loads replace multiplies/clamps
- Intra fill_border: bulk-load guaranteed first-N top/left refs
- IDCT16/32: rightmost non-zero column skip + memset zero tmp
- Deblock chroma: tighter clamp + fewer bound checks
- **SIMD (SSE4.1/SSSE3)**: `hevc_simd.c` — IDCT, residual, color, deblock, dequant,
  fill_border top copy, angular intra (both mode ranges), SAO band + EO H/V.
- `nokia_444` ~121ms → **~51ms ≈ libheif**; full bench: slower on large Nokia grids
  / AVIF / HDR; many small/unci files faster.
- **dav1d asm + AV1 path polish** (`build.ts` stamp `asm=0|1`, nasm auto-detect):
  - Rebuild `libdav1d` with `-Denable_asm=true` (x86 SSE/AVX2/AVX512 objs; ~3.7MB).
  - Reuse cached `Dav1dContext`; `dav1d_flush` after each still; dual
    `dav1d_data_wrap` (av1C configOBUs + sample, no combined malloc).
  - SSE4.1 `plane8_to_u16` for 8-bit plane widen.
  - `example.avif` decode: ~28ms → **~8.9ms** (libheif ~6.2ms multi-thread dav1d).
- **Perf parity polish** (single-thread fair bench + residual/color):
  - libheif oracle: `max_decoding_threads=1` + `num_codec_threads=1` (was 0 → dav1d auto).
  - Residual: one-pass sig list / signs / remaining / store.
  - CABAC: inline `decode_bypass_bits`; color: stack chroma expand (no malloc).
  - `example.heic` ~1.20× → **~1.04×**; `example.avif` ~1.5× → **~1.03×**; `nokia_444` still ≈/≤ libheif.

## Next

- [x] unci (ISO 23001-17): planar/pixel/row/tile-comp; multi-tile; 1..16-bit RGB/mono
- [x] unci: YUV 444/422/420 (planar + tile-comp); default matrix BT.601 like libheif
- [x] unci: uncC `pixel_size` is uint32 (ISO/libheif)
- [x] unci: pixel_size pad on pixel interleave (bsz0_psz5/psz10 fixtures)
- [x] unci: mixed interleave (2) semi-planar 422/420 (YUV/YVU/VUY, 8- and 16-bit)
- [x] unci: zlib/deflate (HEIC_HAVE_ZLIB) + brotli (HEIC_HAVE_BROTLI) + icef multi-unit
- [x] libheif oracle rebuilt with zlib+brotli (generic compressed unci mse=0)
- [x] unci: block_size≠0 (component + pixel packing; flags LE/pad_lsb/reversed)
- [x] unci: row interleave applies row_align per component row (not whole scanline)
- [x] PPS tail: scaling_list skip + lists_mod + slice_header_extension flag
- [x] 4:4:4 chroma QP: Table 8-10 only for 4:2:0 (Min(qPi,51) for 422/444)
- [x] nokia_444 WPP: residual already matched libde265; RGB gap was matrix default
      (VUI without colour_description → BT.601/6 like libheif, not BT.709/1); mse≈0.005
- [ ] soft mse: example/hdr ~5–6 under mse≤8
- [ ] iovl_negoff: oracle black (listed EXPECT_FAIL); ours keeps ISO negative offsets
- [ ] **iovl_negoff**: ours matches ISO/imazen signed-16 offsets; libheif canvas black (oracle disagreement)
- [ ] Scaling lists, PCM
- [x] AVIF grid + alpha (meta/`dimg`/`auxl`; fixtures under `deps/testimages/avif/`)
- [ ] image sequences (`moov`) / HEIF Mini (`mini`) still out of scope

## Investigation notes

### iovl_negoff.heic
- Positive-offset `iovl.heic` is mse=0 vs libheif.
- `iovl_negoff` (offset −16): we produce a 96×96 overlay placement; libheif RGB is all-black.
- Keep as known oracle disagreement; do not “fix” to match black.

### Nokia C021 / C013 (double `dimg` iref)
- `iovl` item references the same `hvc1` id twice (`iref dimg … to [1002,1002]`).
- libheif rejects the whole file: `'iref' has double references`.
- We still open and decode the primary `hvc1` (pitm). Bench: heic timings only, exit 0 / skip compare.

### nokia_grid3x2.heic — FIXED
- Was mse≈13392: slice header `byte_alignment` only ran when not already byte-aligned.
- When the header landed on a byte boundary, CABAC started one byte early → first `split_cu` wrong (0 vs 1), whole CTB desynced, ~half the slice unread.
- Fix (match imazen): always read `alignment_bit_equal_to_one`, then `heic_bs_byte_align` (discard rest of byte).
- After fix: mse≈2.5 maxdiff≈6 vs libheif (loop-filter / SAO residual).

