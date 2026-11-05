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
- extract EXIF (TIFF payload; HEIF offset header skipped), XMP, ICC
- optional thumbnail decode, Apple HDR gain-map decode (no tone map)
- HEVC/AV1 image sequences (timing, one-shot frame decode, stateful playback)

No encoders. No writing HEIF.

**SumatraPDF surface** (see `sumatrapdf/src/shared/AvifReader.cpp` in the
SumatraPDF repo):

| Need | API |
|------|-----|
| size from buffer | `heic_doc_open` + `heic_doc_info` |
| decode to BGRA pixmap | `heic_doc_decode(HEIC_FORMAT_BGRA)` |
| EXIF TIFF blob | `heic_doc_exif` (HEIF offset header already skipped) |

## Architecture

```
src/
  heic.h              public API
  heic_internal.h     single internal header (all modules)
  core.c              ctx, alloc, limits, frame helpers
  heif.c              ISOBMFF / HEIF container + moov sequences (port of imazen/heic src/heif/*)
  document.c          heic_doc open/info/metadata
  decode.c            primary-image + sequence orchestration (grid, iovl, alpha, transforms)
  color.c             YCbCr → RGB (BT.601/709/2020, limited/full)
  hevc_*.c            pure-C HEVC decoder: I/P/B slices, RExt (port of imazen/heic src/hevc/*)
  hevc_inter.c        HEVC motion compensation / weighted prediction
  hevc_simd.c         SSE4.1/SSSE3 kernels (runtime-selected; scalar fallback)
  unci.c              ISO 23001-17 uncompressed images (optional zlib/brotli)
  av1_dav1d.c         thin wrapper: feed OBU/av1C into dav1d, fill heic_frame
```

### Codecs

| Codec | Source | Notes |
|-------|--------|-------|
| **HEVC** (`hvc1`) | Port of **imazen/heic** | Pure C. Intra + inter (P/B), sequences, RExt |
| **AV1** (`av01`) | **dav1d** C library | `HEIC_HAVE_DAV1D` + link dav1d. Do **not** re-port rav1d or rav1d-safe |
| **unci** (`unci`) | `unci.c` | N-bit planar/pixel/row/tile-comp/YUV + block packing; zlib/deflate (`HEIC_HAVE_ZLIB`), brotli (`HEIC_HAVE_BROTLI`) |
| JPEG / H.264 in HEIF | out of scope | Decode fails with generic "unsupported item type" |

### What we do NOT do

- Port rav1d-safe (use dav1d C instead)
- Native GPU backends (MediaFoundation, VideoToolbox, VA-API, …)
- Encode paths
- PQ/HLG EOTF / tone-mapping (expose CICP; caller tone-maps)
- Multilayer HEVC (inter-layer): open/info succeed, decode fails

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

Requires `clang` or MSVC `cl`, `bun`, `git`, `cmake`, `ninja` (zlib/brotli
are auto-built on every `build.ts` run if missing); `meson` for dav1d.
clang/gcc on x86 need `-msse4.1` for `hevc_simd.c` (also for `dist/heic.c`).

The Windows harness vendors `test/winperf_control.h`, so a **winperf** checkout
is not required to compile. To record profiles, first look for the private
winperf repository at `..\winperf`. If it is absent, try to clone it there:

```
git clone https://github.com/kjk/winperf ..\winperf
```

The clone requires access to the private repository. Keep the vendored header
in sync with `..\winperf\client\winperf_control.h`.

```
bun cmd/get-deps.ts      # clone deps + fill deps/testimages (auto-run by build/tests)
bun cmd/build.ts         # build library + heic_test harness (MSVC default on Win)
bun cmd/build.ts -clang  # clang harness
bun cmd/tests.ts -all    # decode + RGB mse vs libheif (fail if we can't / differ)
bun cmd/tests.ts -info -all  # open/probe only
bun cmd/bench.ts -rand 5
bun cmd/fuzz.ts          # libFuzzer + ASan (seeds fuzz/corpus from deps/)
bun cmd/fuzz-afl.ts      # AFL++ on macOS/*nix (shares fuzz/corpus with libFuzzer)
bun cmd/build-dist.ts    # amalgamation → dist/heic.h + dist/heic.c (+ dist/wasm/)
bun cmd/build-wasm.ts    # WebAssembly drop only (deps/emsdk if emcc missing)
bun cmd/run-wasm-demo.ts # serve dist/wasm/demo.html on localhost:8000
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
- Flags: `-jobs N`, `-repro FILE`, `-check-crashes` (CI: replay all
  `fuzz/crashes/*`, expect exit 0), `-minimize` (`-merge=1` corpus shrink),
  `-max-len N`, `-no-deps`. Linux/macOS CI may set `HEIC_FUZZ_UBSAN=1` for
  ASan+UBSan+fuzzer.
- GitHub Actions: `.github/workflows/ci.yml` (Windows MSVC + full oracle tests
  + amalgamation, Windows clang crash regression, Linux clang + full oracle +
  amalgamation + UBSan crashes, macOS clang + full oracle + amalgamation, WASM
  decode smoke). deps/ and oracle build trees under `out/*_build` are cached
  per OS.
- `bun cmd/fuzz-afl.ts` — same harness via **AFL++** (macOS/*nix;
  `brew install afl++`). Shares `fuzz/corpus/` with libFuzzer; AFL state in
  `fuzz/afl-out/` (gitignored). On exit (and with `-import`), queue/crashes
  merge into the shared corpus and `fuzz/crashes/`. Flags: `-jobs N`,
  `-repro FILE`, `-cmin`, `-import`, `-no-asan`, `-V SEC`, `-no-deps`.
  Once per machine on macOS: `sudo afl-system-config` (raises SysV shm limits).
- `fuzz/corpus/` is gitignored; `fuzz/crashes/` is tracked so crash inputs
  become regression seeds. Reproduce with
  `bun cmd/fuzz.ts -repro fuzz/crashes/<artifact>` (or `fuzz-afl.ts -repro`).

WASM: pure-C HEVC + unci only (no `HEIC_HAVE_DAV1D` / zlib / brotli).
`bun cmd/build-dist.ts` also rebuilds split `dist/wasm/heic.js` +
`dist/wasm/heic.wasm` from the amalgamation.

- MSVC default on Windows (`out/msvc/heic_test_msvc.exe`); `-clang` →
  `out/clang/heic_test_clang.exe`.
- Bun's shell eats `\`; normalize `ROOT` to forward slashes; MSVC flags use `-`.
- **Do not commit automatically.** Leave changes in the working tree unless the
  user asks to commit. Never commit `dist/heic.c` / `dist/heic.h` unless asked.

### Amalgamation

`bun cmd/build-dist.ts` → `dist/heic.h` + `dist/heic.c` (sqlite-style).
`dist/heic.h` is `src/heic.h` with whitespace normalized (comments kept). For
`dist/heic.c`: strip local `#include "heic.h"` / `"heic_internal.h"`, remove comments, emit **LF
only**, strip trailing whitespace, and keep at most one blank line in a row.
No two `.c` files may share a `static` symbol name. **Agents do not commit
dist/**.

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
| `hevc_transform.c` | `src/hevc/transform.rs` |
| `hevc_simd.c` | `src/hevc/transform_simd.rs` (+ residual/color/deblock/intra/SAO kernels) |
| `hevc_inter.c` | `src/hevc/inter.rs`, `src/hevc/mc.rs` |
| `hevc_deblock.c` | `src/hevc/deblock.rs` |
| `hevc_sao.c` | `src/hevc/sao.rs` |
| `core.c` (frames) | `heic-core/src/frame.rs` / `src/hevc/picture.rs` |
| `hevc_decode.c` | `src/hevc/mod.rs` entry (`decode_with_config`) |
| `unci.c` | ISO 23001-17 unci decode (checked vs libheif's unc codec) |
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
- **EXIF**: item type `Exif`; first 4 bytes = BE TIFF header offset; TIFF
  starts at `4 + offset`. Items without that header (raw `MM\0*`) are not
  returned (known limitation; `has_exif` still 1).
- **10-bit HEVC → 8-bit RGB**: right-shift `bit_depth-8` (no EOTF).

## Verification methodology

Reference-oracle: compare our RGB (and dimensions/EXIF) against libheif
`heif_decode_image` / metadata APIs on `deps/heic/testdata`. Work
incrementally; keep `PROGRESS.md` current.

False positives are the highest-severity bug: a test that only opens the
container is not a decode test. Drive all the way to pixels.

## Status

Feature-complete for the scope above; remaining gaps and history are in
`PROGRESS.md`.
