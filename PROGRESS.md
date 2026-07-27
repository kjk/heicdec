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
- [x] WebAssembly drop: `dist/wasm/heic.js` + `heic.wasm` + `demo.html` (HEVC/unci; no dav1d)
- [x] Nokia `heif_conformance` in get-deps + corpus (`tests.ts` / `bench.ts` -all)
- [x] Fuzzing: `cmd/fuzz.ts` + `test/fuzz_target.c` (libFuzzer+ASan; `fuzz/corpus/` checkpoint, `fuzz/crashes/` tracked)

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

- MSVC harness builds emit `out/msvc/heic_test_msvc.pdb` (`-Zi` + link `-DEBUG`) for winperf.
- `heic_test -profile-heic N` / `-profile-libheif N` loop decode under winperf section marks.
- Profile-guided wins on HEVC stills (winperf on `nokia_444` 2048×2048):
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
- **Full-corpus winperf pass** (188 files; HDR + C022 profiled against libheif):
  - CABAC bin renormalization: one-shift MPS fast path + table-driven LPS shift.
  - Residual: record significant positions once, index level state by sig-list position,
    batch coefficient signs through `decode_bypass_bits`, inline sig-context helpers.
  - `hdr-sample.heic` 43.5ms → **37.5ms** (-13.7%; libheif 33.0ms).
  - `C022.heic` 122.6ms → **111.1ms** (-9.4%; libheif 109.5ms);
    `C050` -10.6%, `example.heic` -5.7%.
  - After: 166 comparable files / 22 skips / 0 bench failures; pixel oracle
    183 ok / 5 no-oracle skips / 0 failures.
- **CABAC/residual follow-up**:
  - Decode fixed-length bypass bins in chunks of 8 (one refill/divide per chunk).
  - Hoist significant-coefficient context invariants per subblock; replace repeated
    coordinate/branch arithmetic with a local-context table.
  - HDR 37.5ms → **35.4ms** (libheif 34.7ms, +2.1%); normalized winperf CPU
    samples/decode 82.0 → **72.1** (-12.1%).
  - `example.heic` 56.1ms → **50.6ms** (9.3% faster than libheif);
    C022 111.1ms → **109.3ms** (6.1% faster in this corpus pass).
- **Direct 4:2:0 SIMD color**:
  - Load subsampled chroma directly into duplicated SIMD lanes, including odd
    crop phase; remove per-decode temp allocation and scalar 4:2:0→4:4:4 expansion.
  - HDR long-loop 35.0ms → **33.8ms**; normalized winperf samples/decode
    72.1 → **67.3** (-6.6%).
  - Full pass: HDR 34.2ms vs libheif 32.8ms (+4.3%); C022 98.8ms vs
    libheif 103.9ms (-4.9%); 166 comparable / 22 skips / 0 failures.
- **Crop before rotation**:
  - Materialize an aligned clean-aperture crop before `irot`, preserving chroma
    phase and retaining the full-frame transform as a fallback.
  - C014 25.9ms → **24.6ms** (libheif 25.7ms); C039 27.0ms → **25.4ms**
    (libheif 27.1ms). Winperf no longer reports rotation among the top functions
    (previously 3.4% self / 5.4% inclusive on C014).
  - Full pass remains 166 comparable / 22 skips / 0 failures; pixel oracle
    183 ok / 5 no-oracle skips / 0 failures.
- **Direct full-range 4:4:4 SIMD color**:
  - Replace twenty scalar LUT reads per four pixels with three packed sample
    loads and direct SSE4.1 fixed-point multiplies.
  - Alternating 100-decode A/B: 49.64ms → **48.25ms** median (-2.8%) on
    `nokia_444`; clean paired bench 47.11ms vs libheif 47.74ms (-1.3%).
  - Full pass remains 166 comparable / 22 skips / 0 failures; pixel oracle
    183 ok / 5 no-oracle skips / 0 failures.
- **Direct 8-bit unci planar rows**:
  - Widen byte-aligned component rows directly into frame planes instead of
    routing every sample through the generic bit reader, scaler, and bounds check.
  - Zlib fixture 0.30ms → **0.09ms**; deflate 0.26ms → **0.09ms**; Brotli
    0.30ms → **0.12ms**. All compressed unci fixtures are now at parity or faster.
  - Winperf 20k-decode workload 4.60s → **0.87s** (-81.0%); full pass remains
    166 comparable / 22 skips / 0 failures and 183 pixel-oracle successes.
- **Precombined residual scan contexts**:
  - Replace per-coefficient scan-coordinate/context arithmetic with compact
    scan-order/context tables; fold level initialization, sign traversal, and
    nonzero accounting into existing passes.
  - HDR winperf 34.34ms → **33.23ms** (-3.2%); residual self-samples
    2,775 → **1,925** (-30.6%). Focused bench: HDR 32.88ms vs libheif
    33.89ms (-3.0%).
  - Full pass remains 166 comparable / 22 skips / 0 failures; pixel oracle
    183 ok / 5 no-oracle skips / 0 failures.
- **Uninitialized packed output allocation**:
  - Allocate RGB/BGR/RGBA/BGRA output without clearing it first; color
    conversion overwrites every output byte.
  - HDR repeated median 31.78ms → **31.29ms** (-1.5%); winperf caller
    attribution removed all 246 `memset` samples charged to primary decode.
  - Full pass remains 166 comparable / 22 skips / 0 failures; pixel oracle
    183 ok / 5 no-oracle skips / 0 failures.
- **Active-size residual scratch clearing**:
  - Clear only the active 4x4 through 32x32 coefficient square instead of the
    full 1,024-coefficient scratch buffer for every transform.
  - HDR winperf 33.52ms → **31.55ms** (-5.9%); normalized samples/decode
    66.5 → **62.8** (-5.6%), with residual self-samples down 7.1%.
  - Full pass remains 166 comparable / 22 skips / 0 failures; pixel oracle
    183 ok / 5 no-oracle skips / 0 failures.
- **No redundant intra border clear**:
  - Stop zeroing the full intra border scratch array before `fill_border`,
    which already writes or substitutes every active reference sample.
  - HDR repeated median 32.33ms → **30.93ms** (-4.3%); winperf eliminated
    the separate small-`memset` entry (186 samples across 200 decodes).
  - Full pass remains 166 comparable / 22 skips / 0 failures; pixel oracle
    183 ok / 5 no-oracle skips / 0 failures.

## Next

### Remaining priorities (ordered)

- [x] Official motion-vector clipping/edge streams: `MVCLIP_A_qualcomm_3`
      (5 frames) and `MVEDGE_A_qualcomm_3` (17 frames) install via get-deps
      and match pinned reconstruction MD5s byte-for-byte vs libde265.
- [x] `MVHEVCS_*` folded into multilayer out-of-scope (needs multilayer PPS).
- [x] HEVC Main 4:2:2 / Main 4:2:2 10: dual vertical chroma CBF/TUs, SubHeightC=1
      plane layout, and 4:2:2 chroma IntraPredModeC remapping (H.265 8.4.3).
      Official `GENERAL_10b_422` sequences match pinned MD5s byte-for-byte;
      `Main_422_10_A_RExt_Sony_2` (9 frames) matches libde265 at mse≈0.002.
- [x] Strict corpus verdicts: `both_fail` is a real `[fail]` unless the file is
      in `EXPECT_FAIL` (malformed/intentional) or `OUT_OF_SCOPE` (skip). Nokia
      multilayer001–005 are out of scope; must-decode fixtures still fail hard.
- [x] Expanded raw HEVC conformance by feature family (FATE streams, exact MD5
      vs libde265): DELTAQP_C; RQT_A–C + TUSIZE + MAXBINS_A–C; IPRED_A–C +
      CIP_A–C; SAO_A–G; TILES_A/B; POC_A; RPS_A/B/C/E/F; WPP Main A/B/C/E/F
      (48-frame Ericsson streams). Fix: dependent slice headers still parse
      entry-point offsets + header extension (H.265 7.3.6.1). Remaining gaps:
      WPP_D (single-CTB-wide), WPP Main10 (P-frame WPP desync), DELTAQP_A/B,
      NoOutPrior/RAP DPB output order, RPS_D, SLIST, PERSIST_RPARAM.
- [x] Enforce `heic_limits.max_memory_bytes`: size-header tracked alloc/free
      (`heic_alloc` / `heic_zalloc` / `heic_free_buf`), overflow-safe cap checks,
      frame planes and RGB output routed through them; `heic_test -memory-limit`
      regression (tiny cap rejects frame alloc; free restores budget).
- [x] Multilayer HEVC stills classified out of scope (`OUT_OF_SCOPE` skip set).
      Inter-layer / VPS-layer decode remains optional future work.
- [x] Feature-family HEVC sequences installed via get-deps (FATE) with pinned
      reconstruction MD5s for DELTAQP_C, RQT, TUSIZE, MAXBINS, IPRED, CIP,
      SAO_A–G, TILES, POC_A, RPS_A/B/C/E/F.
- [x] Strengthen memory-safety automation:
      - `bun cmd/fuzz.ts -check-crashes` replays every `fuzz/crashes/*` under ASan
      - `HEIC_FUZZ_UBSAN=1` enables ASan+UBSan+fuzzer on Linux/macOS
      - GitHub Actions `.github/workflows/ci.yml`: Windows MSVC release smoke,
        Windows clang crash regression, Linux clang+UBSan crashes + amalgamation,
        WASM decode smoke
- [ ] Resume performance work after the correctness sweep. Profile the new
      4:2:2 and motion-vector streams plus the full HEIF corpus; then target
      measured hot paths with AVX2/ARM64 NEON and consider parallel decoding
      of independent grid tiles. Keep scalar output byte-identical.
- [x] Perf restart (post-correctness): HDR `hdr-sample` 40.3ms → **37.2ms**
      (libheif ~34.6ms, +7.4%; was +19.6%). Wins: stack color LUTs (no per-frame
      malloc), zero-skip dequant (plain + scaling-list), direct full-range 4:2:0
      SIMD multiplies (no chroma LUT gathers). Skipping plane UNINIT fill was
      tried and reverted (breaks color on unread samples).
- [x] Perf follow-up (SIMD sparse transform + limited color):
      - SIMD dequant skips all-zero 8-packs (scalar zero-skip was dead when SIMD ran).
      - IDCT8/16 skip zero column groups like IDCT32; residual `num_nonzero`
        drives pure-DC IDCT without scanning the block.
      - Limited-range 4:4:4 / 4:2:0 SIMD uses clamp + fixed-point multiplies
        (same scales as LUTs) instead of per-pixel LUT gathers.
      - 4:2:0 dual-row color: even/odd luma pairs share one chroma multiply pass.
- [x] I-slice setup trim (grid stills):
      - Skip `mv_info` / `mc_internal` on I-only pictures; lazy-allocate if a
        later P/B segment appears. Skip `pred_mode_map` unless constrained-intra
        or inter. Use single-fill `heic_alloc`+memset for ct_depth / intra modes /
        QP maps (no zero-then-overwrite).
      - HDR ~+5–8% vs libheif (was ~+10%); `nokia_444` often faster than libheif;
        `example.heic` still ~−9%. Inter MD5s (MVCLIP/AMVP/CIP) unchanged.
- [x] Grid tile buffer reuse + chroma blit:
      - `heic_frame_prepare` reuses equal-size plane buffers (HDR 512² tiles
        allocate once, re-UNINIT). HEVC decode no longer `memset`s away planes.
      - Chroma tile blit uses row `memcpy` like luma.
      - HDR ~+2–5% vs libheif (was ~+8%); `example` ~−11%; `nokia_444` ≈/≤.
- [x] CABAC init tables + hvcC param cache:
      - Precompute all CABAC context init states for I/P/B × QP 0–51 (memcpy
        per slice instead of 174× arithmetic).
      - Cache SPS/PPS parsed from hvcC on `heic_ctx` so grid tiles skip reparse;
        sample-stream param NALs invalidate the cache.
      - HDR often ~+2–4% vs libheif; `example` still faster; oracle OK.
- [x] I-slice picture scratch reuse (single-thread, no tiles parallel):
      - Cache finished I-slice `heic_hevc_picture` maps on `heic_ctx`; next
        equal-size tile resets maps in place instead of free/realloc.
      - HDR ~+2–4% vs libheif; MVCLIP/AMVP MD5s unchanged.
- [x] CABAC bin force-inline + I-slice deblock + scan inverse tables:
      - `hevc_cabac_inline.h`: `__forceinline` decode_bin/bypass used by
        residual + CTU (no call overhead per CABAC bin).
      - I-slice deblock skips `compute_bs` (bS always 2 when pred_mode=NULL).
      - Residual last-sig scan uses O(1) inverse tables for 4x4 / 2x2 scans.
      - HDR often ~+0–3% vs libheif (noise); `example` ~−12%; oracle OK.
- [x] SAO plane reuse + bypass_bits inline + deblock QP fill:
      - Edge-SAO orig plane buffers cached on `heic_ctx` (no free/alloc per tile);
        skip SAO entirely when all CTB types are off.
      - Inline `decode_bypass_bits` into residual/CTU; `heic_store_deblock_qp`
        uses row `memset`.
      - HDR still ~+0–3% vs libheif; oracle OK.
- [x] Zero-copy NAL RBSP when no EPB + 8×8 scan inverse:
      - `heic_parse_single_nal` skips alloc/copy when payload has no 0x000003;
        unescape uses non-zeroing alloc when copy is required.
      - Residual last-sig uses O(1) inverse for 8×8 subblock diagonal scan.
      - HDR often ≈/≤ libheif in noise; example still faster.
- [x] Prepare a release-quality integration pass: exercise the SumatraPDF
      buffer/size/BGR/EXIF workflow, add public-API allocator and abort tests,
      rebuild/verify the amalgamation and WASM drop, and document supported
      profiles plus intentional exclusions.
      - `heic_test -sumatra` / `-api` / `-memory-limit`
      - `bun cmd/verify-release.ts` (orchestrates + build-dist + verify-wasm)
      - README: Sumatra surface table + supported/excluded profiles
      - amalgamation embeds `hevc_cabac_inline.h` once (no duplicate statics)
      - MSVC links dav1d (and libheif) with `/MD` so `__imp__aligned_malloc` /
        CRT match meson-built static libs; feature stamp tracks `crt=MD|MT`

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
- [x] HEVC scaling lists (SPS/PPS default, predicted, and explicit matrices);
      `apple-hdr/hdr-sample.heic` exercises scaling-list-enabled decoding
- [x] HEIF Mini v0 (integer HEVC/AV1, alpha, orientation, NCLX, ICC, EXIF/XMP);
      official `hevc32-mini.heif` and `avif32-mini.heif` fixtures
- [x] HEVC predictive image items: single-reference P pictures with merge/AMVP,
      fractional motion compensation, residuals, deblock, and SAO; Nokia `C044.heic`
- [x] HEVC inline short-term and long-term reference-picture-set syntax; retain
      SPS and slice long-term POC/use/delta-cycle fields for reference-list construction
- [x] HEVC multiple L0 references: recursive `pred` dependencies, POC-based
      StCurrBefore/StCurrAfter/LtCurr lists, list modification, and CABAC `ref_idx_l0`
- [x] HEVC temporal MVP for P pictures: retained per-PU motion state, collocated
      bottom-right/center candidates, and POC-distance scaling for merge/AMVP
- [x] HEVC weighted P prediction: full luma/chroma weight-table syntax,
      derived chroma offsets, internal-precision normative equations,
      bit-depth scaling, rounding, and clipping
- [x] HEVC B slices: L0/L1 RPS construction and list modification, B CABAC
      motion syntax, spatial/temporal AMVP, combined bi-merge candidates,
      L1/bi motion compensation, and internal-precision weighted biprediction
- [x] Raw Annex B HEVC sequence regression path with `dec265` pixel oracle;
      FATE fixtures cover long-term RPS, RPLM, TMVP, weighted P/B, and B merge
- [x] HEVC transform-skip conformance: all 17 frames of `TSKIP_A_MS_3` match
      libde265 at mse 0.002; all 8 `TSUNEQBD_A_MAIN10_Technicolor_2` frames
      match at mse 0.009, including independent 10-bit luma / 9-bit chroma
      reconstruction, inter prediction, and RGB
- [x] HEVC Range Extensions transform tools: full VUI/SPS/PPS extension
      parsing, larger transform skip, RExt significant-coefficient contexts,
      coefficient rotation, implicit RDPCM, intra smoothing disable, and
      persistent Rice state across WPP rows. Official `TSCTX` (3 frames) and
      `GENERAL` sequence 1-5 feature isolates match libde265 byte-for-byte; a
      generated 4:4:4 HEIF wrapper is covered by the normal libheif corpus.
- [x] HEVC cross-component prediction: RExt CABAC scale syntax, retained luma
      transform residuals, chroma prediction when CBF is zero, and bit-depth
      normalization for transform, transform-skip/RDPCM, and transquant-bypass
      paths. All nine frames of official `CCP_8bit_RExt_QCOM_1` match libde265
      at mse 0.0010 / maxdiff 4; the first intra picture is byte-for-byte exact.
- [x] HEVC RExt CU chroma QP offset lists: slice enable syntax, CABAC flag/index,
      chroma QP group state, and per-CU Cb/Cr offsets. Official `GENERAL`
      sequence 6 matches libde265 byte-for-byte; a generated 4:4:4 HEIF wrapper
      is covered by the normal libheif corpus.
- [x] HEVC RExt CABAC bypass alignment: detect coefficient groups carrying
      remaining-level syntax and switch to the aligned range before sign and
      escape bins. The first official `EXTPREC_HIGHTHROUGHPUT` picture matches
      the published HM reconstruction MD5 byte-for-byte.
- [x] HEVC RExt extended-precision processing: decode limited-prefix escape
      levels, retain the expanded transform coefficient range through inverse
      quantization, and use wide first-pass transform arithmetic. The second
      official `EXTPREC_HIGHTHROUGHPUT` picture matches the published HM
      reconstruction MD5 byte-for-byte.
- [x] HEVC RExt high-precision weighted prediction: validate bit-depth-sized
      luma/chroma offset ranges, derive chroma offsets around the extended
      sample midpoint, and apply offsets without 8-bit scaling. A compact
      four-frame 10-bit HM P sequence with nonzero Y/Cb/Cr offsets matches the
      HM reconstruction MD5 byte-for-byte.
- [x] HEVC RExt SAO offset scaling: apply the independently signaled PPS
      luma/chroma shifts instead of deriving a shift from bit depth. A compact
      12-bit HM intra sequence with shifts 1/2 matches the HM reconstruction
      MD5 byte-for-byte.
- [x] HEVC constrained intra prediction: exclude inter- and skip-coded
      neighboring min-PU regions from luma/chroma intra reference samples. A
      compact two-frame HM P sequence with mixed prediction modes matches the
      HM reconstruction MD5 byte-for-byte.
- [x] HEVC tile/slice boundaries: keep CABAC, intra prediction, SAO merge and
      SAO/deblocking samples within their independently coded region, and
      honor the PPS/slice loop-filter boundary controls. Compact HM tile and
      x265 slice streams with cross-boundary filtering disabled match HM
      reconstruction MD5s byte-for-byte.
- [x] HEVC per-slice deblocking: select beta/tC from the q-side slice for
      every luma/chroma edge, while applying only the PPS chroma QP offsets
      required by the deblocking process. The first `DBLK_A_SONY_3` frame,
      which varies filter offsets by slice, matches libde265 exactly.
- [x] HEVC luma boundary strength: only nonzero luma coefficients raise the
      strength of a transform edge; chroma-only residuals do not. All 30
      frames of official `DBLK_B_SONY_3` and `DBLK_C_SONY_3` match their
      reconstruction MD5s byte-for-byte.
- [x] HEVC prediction-block availability excludes not-yet-decoded NxN
      partitions inside the current coding unit for both merge and AMVP
      candidates. All eight 10-bit frames of official
      `DBLK_A_MAIN10_VIXS_3` match the reconstruction MD5 byte-for-byte.
- [x] HEVC advanced motion-vector prediction is covered across official
      `AMVP_A_MTK_4`, `AMVP_B_MTK_4`, and `AMVP_C_Samsung_6`. All 131
      frames match both HM and libde265 byte-for-byte.
- [x] HEVC motion-vector clipping and edge cases: official
      `MVCLIP_A_qualcomm_3` (5 frames) and `MVEDGE_A_qualcomm_3` (17 frames)
      match pinned reconstruction MD5s byte-for-byte.
- [x] HEVC Main 4:2:2 10: dual vertical chroma CBF/TUs (SubHeightC=1),
      4:2:2 chroma IntraPredModeC remap table, official `GENERAL_10b_422`
      (exact MD5) and `Main_422_10_A_RExt_Sony_2` (9 frames, mse≈0.002)
- [x] HEVC parallel merge regions use one CU-wide candidate list for every
      sub-partition of an 8x8 coding unit when the signaled merge level is
      larger than 4x4. All eight frames of official `PMERGE_A` through
      `PMERGE_E` match their reconstruction MD5s byte-for-byte.
- [x] HEVC picture-order MSB inference uses the previous temporal-id-0
      reference picture and only resets for BLA/IDR pictures. All 500 frames
      of official `MVDL1ZERO_A_docomo_3`, including a midstream CRA and its
      leading RASL pictures across an LSB wrap, match the reconstruction MD5
      byte-for-byte.
- [x] HEVC transform-skip rotation and implicit RDPCM are covered by official
      `WPP_HIGH_TP_444_8BIT_RExt_Apple_2`. All three frames match HM and
      FFmpeg byte-for-byte; the test uses the reconstruction MD5 because
      libde265 incorrectly suppresses rotation for subsampled chroma.
- [x] HEVC deblocking QP propagation: when a CU QP delta is decoded after
      earlier transform units, retroactively update the whole coding unit's
      deblock QP map. All eight frames of official `DBLK_F_VIXS_1` and
      `DBLK_F_VIXS_2` match their reconstruction MD5s byte-for-byte.
- [x] HEVC tile QP prediction: reset the QP predictor at each tile's first
      quantization group, including tiles that begin with a dependent slice
      segment. All eight frames of official `DBLK_D/E/G_VIXS_2` match their
      reconstruction MD5s byte-for-byte.
- [x] HEVC intra references across non-rectangular slice boundaries: evaluate
      top/left extensions and the top-left corner per sample before normative
      substitution. Official `SAO_H_Parabola_1` (four frames, diagonal SAO at
      slice corners) matches its published reconstruction MD5 exactly.
- [x] HEVC bidirectional MC retains 14-bit internal samples through blending
- [x] HEVC merge deduplication and deblocking compare resolved reference
      pictures, including cross-list B prediction order
- [x] HEVC sequence oracles share the normal MSE <= 8 gate; corrected spatial
      merge-slot tracking, zero-MV candidate indices, and long-term MV scaling
- [x] All libheif-rejected corpus files use compact alternate RGB pixel
      oracles from imazen/heic or libde265; full tests have zero oracle skips
- [x] HEVC image sequences (`moov`): first sync sample from `pict` (`vide`
      fallback), `stsd`/`stsz`/`stsc`/`stco`/`co64`/`stss`, and a smaller
      `pict` thumbnail track; Nokia `C026`–`C032`, `C036`–`C038`, and `C041`
- [x] Public HEVC sequence API: `stts`/`ctts` timing, `elst` presentation
      edits, finite/infinite repetition counts, and arbitrary frame decode
      from the preceding sync sample; all 11 Nokia sequence fixtures covered
- [x] Every public HEVC sequence frame is compared in RGBA against libheif's
      sequence API/libde265, including edit-list duplicates and hidden preroll
- [x] Stateful HEVC sequence playback retains reconstructed pictures from the
      active sync sample, avoiding repeated preroll decode and supporting
      long-term reference candidates beyond the 16-entry active reference list
- [x] Apple HDR auxiliary gain maps can be decoded independently as RGB/BGR(A)
      pixels without tone mapping; the bundled Apple fixture is compared
      directly against libheif's auxiliary-image decode
- [x] AV1 timed image sequences (`av01` tracks / `avis` brand) use persistent
      dav1d state for dependent samples; libavif's five-frame inter-coded
      fixture covers show-existing frames, timestamp-associated delayed output,
      reset/random access, and every RGB frame against libheif/dav1d
- [x] Combined `meta` + `moov` image sequences preserve the still primary item
      and its metadata while exposing the movie track through sequence APIs;
      the original libavif fixture is tested unchanged
- [x] Animated AVIF alpha tracks: parse `auxv` sample entries with alpha
      `auxi` and `tref/auxl`, retain independent color/alpha dav1d state,
      synchronize presentation timelines, and compare every RGBA frame against
      libheif using libavif's alpha + EXIF/XMP sequence fixture
- [ ] soft mse: example/hdr ~5–6 under mse≤8
- [ ] iovl_negoff: oracle black (listed EXPECT_FAIL); ours keeps ISO negative offsets
- [ ] **iovl_negoff**: ours matches ISO/imazen signed-16 offsets; libheif canvas black (oracle disagreement)
- [x] HEVC PCM: raw luma/chroma samples, CABAC restart, bit-depth scaling,
      and PCM loop-filter exclusion; official `ipcm_A/B/C_NEC_3` oracles mse=0
- [x] HEVC dependent slice segments: inherit the owning independent header and
      CABAC models while restarting arithmetic state; retain picture maps across
      segments, enforce independent-slice neighbor boundaries, and delay
      deblock/SAO until the picture is complete. All 50 reordered I/B pictures
      in official `DSLICE_A_HHI_5` match libde265 at mse=0.000082, maxdiff=3;
      a generated HEIC wrapper is also covered by the normal libheif corpus.
- [x] HEVC dependent slices with WPP: dynamic Annex B NAL collection handles
      the 1,700-segment official `DSLICE_B_HHI_5` stream; all 50 reordered I/B
      pictures match libde265 at mse=0.000142, maxdiff=3, with a generated HEIC
      wrapper also covered by the normal corpus.
- [x] HEVC transquant bypass: decode the CU flag before `cu_skip_flag` and
      exclude bypassed samples from deblock/SAO. All nine pictures in official
      `LS_A_Orange_2` match libde265 byte-for-byte, with a generated HEIC
      wrapper covered by the normal corpus.
- [x] AVIF grid + alpha (meta/`dimg`/`auxl`; fixtures under `deps/testimages/avif/`)

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
