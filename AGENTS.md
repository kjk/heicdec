# AGENTS.md — working on the C HEIC/HEIF/AVIF decoder

Plain-C, decode-only HEIC/HEIF/AVIF library. Container + HEVC are ported from
the pure-Rust **imazen/heic** project. AV1 uses **videolan dav1d** (C) directly
— do **not** port rav1d-safe. Verification oracle: **libheif** (+ dav1d).
API style follows **jbig2dec** / **djvudec**.

## Goal / scope

Decode-only. The caller hands us the entire `.heic`/`.heif`/`.avif` file
up-front as an in-memory buffer (no incremental fetch). We provide:

- probe primary image info (dimensions, bit depth, alpha, CICP, EXIF/XMP flags)
- decode primary image to RGB / BGR / RGBA / BGRA (8-bit output)
- extract EXIF (TIFF payload, HEIF 4-byte prefix stripped), XMP, ICC
- optional thumbnail decode

No encoders. No writing HEIF.

**SumatraPDF surface** (see `C:\Users\kjk\src\sumatrapdf\src\AvifReader.cpp`):

| Need | API |
|------|-----|
| size from buffer | `heic_doc_open` + `heic_doc_info` |
| decode to BGRA-ish pixmap | `heic_doc_decode(HEIC_FORMAT_RGB)` then convert, or `HEIC_FORMAT_BGR` |
| EXIF TIFF blob | `heic_doc_exif` (prefix already stripped) |

## Architecture

```
src/
  heic.h              public API
  heic_internal.h     single internal header (all modules)
  core.c              ctx, alloc, limits, frame helpers
  heif.c              ISOBMFF / HEIF container (port of imazen/heic src/heif/*)
  document.c          heic_doc open/info/metadata
  decode.c            primary-image orchestration (grid, alpha, transforms)
  color.c             YCbCr → RGB (BT.601/709/2020, limited/full)
  hevc_*.c            pure-C HEVC I-frame decoder (port of imazen/heic src/hevc/*)
  av1_dav1d.c         thin wrapper: feed OBU/av1C into dav1d, fill heic_frame
```

### Codecs

| Codec | Source | Notes |
|-------|--------|-------|
| **HEVC** (`hvc1`) | Port of **imazen/heic** | Pure C. Still-image I-slices first; inter optional later |
| **AV1** (`av01`) | **dav1d** C library | Link/vendor dav1d. Do **not** re-port rav1d or rav1d-safe |
| JPEG / H.264 in HEIF | out of scope | Detect and error clearly |

### What we do NOT do

- Port rav1d-safe (use dav1d C instead)
- Native GPU backends (MediaFoundation, VideoToolbox, VA-API, …)
- Encode paths (unci: zlib/deflate/brotli + N-bit planar/pixel/row/tile-comp/YUV + block packing supported)
- PQ/HLG EOTF / tone-mapping (expose CICP; caller tone-maps)

## Reference checkouts (local) — corpus + oracles

`bun cmd/get-deps.ts` clones into `deps/` (gitignored):

| Path | Repo | Role |
|------|------|------|
| `deps/heic` | https://github.com/imazen/heic | Rust source being ported; **testdata/** is primary corpus |
| `deps/dav1d` | https://code.videolan.org/videolan/dav1d | AV1 decoder (C) we link; also oracle / inspiration |
| `deps/libheif` | https://github.com/strukturag/libheif | Pixel/oracle (static cmake build) + API inspiration |
| `deps/libde265` | https://github.com/strukturag/libde265 | HEVC backend for libheif oracle |
| `deps/heif_conformance` | https://github.com/nokiatech/heif_conformance | Nokia HEIF conformance candidates (`conformance_files/C*.heic`) |
| `deps/zlib`, `deps/brotli` | madler/zlib, google/brotli | unci generic compression |
| `deps/testimages` | link-u fox AVIFs + generators | AVIF grid/alpha + unci block fixtures (not checked in) |

Corpus (used by `tests.ts` / `bench.ts` via `cmd/corpus.ts`):
- `deps/heic/testdata/**`
- `deps/libheif/examples/*.{heic,avif}`
- `deps/heif_conformance/conformance_files/*.heic`
- `deps/testimages/avif/**`, `deps/testimages/unci_block/**`

Override with `HEIC_SPECS=<dir>`.

**Script convention:** every corpus script does nothing by default — prints usage
+ file count. Select with `file.heic …`, `-rand N`, or `-all`.

## Build & test

Requires `clang` or MSVC `cl`, `bun`, `git`.

```
bun cmd/get-deps.ts      # clone deps + fill deps/testimages (auto-run by build/tests)
bun cmd/build.ts         # build library + heic_test harness (MSVC default on Win)
bun cmd/build.ts -clang  # clang harness
bun cmd/tests.ts -all    # decode + RGB mse vs libheif (fail if we can't / differ)
bun cmd/tests.ts -info -all  # open/probe only
bun cmd/bench.ts -rand 5
bun cmd/fuzz.ts          # libFuzzer + ASan (seeds fuzz/corpus from deps/)
bun cmd/build-dist.ts    # amalgamation → dist/heic.h + dist/heic.c (+ wasm/heic.js)
bun cmd/build-wasm.ts    # WebAssembly drop only (deps/emsdk if emcc missing)
bun cmd/verify-wasm.ts <file.heic>
```

### Fuzzing (memory-safety)

- `bun cmd/fuzz.ts` — coverage-guided fuzzing (libFuzzer + ASan).
  `test/fuzz_target.c` opens each input as a HEIC/HEIF/AVIF, probes info,
  decodes primary (+ thumbnail), and pulls EXIF/XMP/ICC, with a budgeted
  allocator (256 MB live). Builds into `out/fuzz/heic_fuzz.exe` via
  `buildFuzz()`. First run seeds `fuzz/corpus/` from the deps/ corpus (files
  over `-max-len` skipped); the corpus dir **is** the checkpoint — kill to
  stop, rerun to resume. Auto-builds dav1d/zlib/brotli when missing so AVIF +
  unci compression are covered (`-no-deps` for pure-C HEVC only).
- Flags: `-jobs N`, `-repro FILE`, `-minimize` (`-merge=1` corpus shrink),
  `-max-len N`, `-no-deps`.
- `fuzz/corpus/` is gitignored; `fuzz/crashes/` is tracked so crash inputs
  become regression seeds. Reproduce with
  `bun cmd/fuzz.ts -repro fuzz/crashes/<artifact>`.

WASM: pure-C HEVC + unci only (no `HEIC_HAVE_DAV1D` / zlib / brotli).
`bun cmd/build-dist.ts` also rebuilds `wasm/heic.js` from the amalgamation.

- MSVC default on Windows (`out/msvc/heic_test_msvc.exe`); `-clang` →
  `out/clang/heic_test_clang.exe`.
- Bun's shell eats `\`; normalize `ROOT` to forward slashes; MSVC flags use `-`.
- **Do not commit automatically.** Leave changes in the working tree unless the
  user asks to commit. Never commit `dist/heic.c` / `dist/heic.h` unless asked.

### Amalgamation

`bun cmd/build-dist.ts` → `dist/heic.h` + `dist/heic.c` (sqlite-style). Strip
local `#include "heic.h"` / `"heic_internal.h"`. No two `.c` files may share a
`static` symbol name. **Agents do not commit dist/**.

AV1 note: amalgamation is the **our** code (HEIF + HEVC + color + orchestration).
dav1d stays a separate library the consumer links (or a separately vendored
tree), matching how SumatraPDF links dav1d today.

## C API (`src/heic.h`) — jbig2dec flavor

Opaque `heic_ctx` / `heic_doc`; caller `heic_alloc_cb` / `free_cb` / `error_cb`.
`heic_init()` once before threads. Key calls:

```c
heic_ctx *ctx = heic_ctx_new(NULL, NULL, NULL, NULL);
heic_doc *doc = heic_doc_open(ctx, data, len);
heic_image_info info; heic_doc_info(doc, &info);
heic_image *img = heic_doc_decode(doc, HEIC_FORMAT_RGB);
uint8_t *exif; size_t n; heic_doc_exif(doc, &exif, &n);
```

## C → Rust source map (imazen/heic)

Paths relative to `deps/heic/`.

| C (`src/`) | Rust |
|------------|------|
| `heif.c` | `src/heif/boxes.rs`, `src/heif/parser.rs` |
| `document.c` | probe/`ImageInfo` parts of `src/lib.rs`, metadata extract |
| `decode.c` | `src/decode.rs` (grid, overlay, alpha, transforms, orchestration) |
| `color.c` | `heic-core/src/color_convert.rs`, `src/hevc` color paths |
| `hevc_bitstream.c` | `src/hevc/bitstream.rs` |
| `hevc_params.c` | `src/hevc/params.rs` |
| `hevc_cabac.c` | `src/hevc/cabac.rs` |
| `hevc_slice.c` | `src/hevc/slice.rs` |
| `hevc_ctu.c` | `src/hevc/ctu.rs` |
| `hevc_intra.c` | `src/hevc/intra.rs` |
| `hevc_residual.c` | `src/hevc/residual.rs` |
| `hevc_transform.c` | `src/hevc/transform.rs` (+ SIMD later) |
| `hevc_deblock.c` | `src/hevc/deblock.rs` |
| `hevc_sao.c` | `src/hevc/sao.rs` |
| `hevc_frame.c` | `heic-core/src/frame.rs` / picture |
| `hevc_decode.c` | `src/hevc/mod.rs` entry (`decode_with_config`) |
| `av1_dav1d.c` | **not** a port of rav1d-safe — wraps dav1d C API for still OBUs |

Decode-only: skip all encoder paths.

## AV1 via dav1d (do not re-port)

1. From HEIF: read `av1C` + item data (OBU stream).
2. Create `Dav1dContext`, set `frame_size_limit` from `heic_limits`.
3. `dav1d_data_wrap` + `dav1d_send_data` / `dav1d_get_picture`.
4. Copy planar YUV into `heic_frame`, then `heic_frame_to_rgb`.

Oracle: same file through libheif (dav1d plugin) must match closely.

Inspiration: dav1d `include/dav1d/dav1d.h`, tools; libheif `libheif/plugins/decoder_dav1d*`.

## Format cheat-sheet

- **Container**: ISOBMFF boxes — `ftyp`, `meta` (fullbox), `pitm`, `iloc`,
  `iinf`/`infe`, `iprp`/`ipco`/`ipma`, `iref`, `idat`, `mdat`. Brands:
  `heic`/`heix`/`mif1`/`avif`/`avis`/`msf1`/…
- **Grid**: `grid` item + `dimg` refs to tiles; assemble by ispe + tile ispe.
- **Alpha**: `auxl` + `auxC` URN; decode aux plane, attach as A.
- **hvcC**: length_size + VPS/SPS/PPS NAL arrays; slices length-prefixed.
- **av1C**: configOBUs prepended before sample OBUs for dav1d.
- **EXIF**: item type `Exif`; first 4 bytes = TIFF offset within payload —
  strip them before returning TIFF.
- **10-bit HEVC → 8-bit RGB**: right-shift `bit_depth-8` (no EOTF).

## Verification methodology

Reference-oracle: compare our RGB (and dimensions/EXIF) against libheif
`heif_decode_image` / metadata APIs on `deps/heic/testdata`. Work
incrementally; keep `PROGRESS.md` current.

False positives are the highest-severity bug: a test that only opens the
container is not a decode test. Drive all the way to pixels.

## Status

Bootstrap in progress. See `PROGRESS.md`.
