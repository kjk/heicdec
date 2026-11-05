# heic — a plain-C HEIC/HEIF/AVIF decoder

Decode-only library for [HEIC/HEIF](https://fileformats.dev/heic) and [AVIF](https://fileformats.dev/avif) still images, aimed at easy embedding
(e.g. [SumatraPDF](https://www.sumatrapdfreader.org/)):

* plain C, jbig2dec-style API
* **HEVC**: pure-C port of [imazen/heic](https://github.com/imazen/heic)
* **AV1**: [videolan dav1d](https://code.videolan.org/videolan/dav1d) (C; linked, not re-ported)
* HEVC and AV1 image sequences: presentation timing, edit-list loops, one-shot
  frame decode, and stateful playback with persistent codec state
* Apple HDR auxiliary gain-map extraction (component decode only; no tone map)
* simple drop-in amalgamation: `dist/heic.h` + `dist/heic.c` (see
  [Using the amalgamation](#using-the-amalgamation) for flags and optional codecs)
* verified against [libheif](https://github.com/strukturag/libheif)

## API

See [`src/heic.h`](src/heic.h). Sketch:

```c
heic_init();
heic_ctx *ctx = heic_ctx_new(NULL, NULL, NULL, NULL);
heic_doc *doc = heic_doc_open(ctx, data, len);   /* data must outlive doc */
heic_image_info info;
heic_doc_info(doc, &info);
heic_image *img = heic_doc_decode(doc, HEIC_FORMAT_RGB);
uint8_t *exif; size_t n;
if (heic_doc_exif(doc, &exif, &n)) { /* TIFF payload */ heic_free(ctx, exif); }
heic_image_destroy(ctx, img);
heic_doc_close(doc);
heic_ctx_free(ctx);
```

### Using the amalgamation

Compile `dist/heic.c` as one C11 translation unit next to `dist/heic.h`.

* **x86/x64 with clang or gcc:** add `-msse4.1`. The SIMD paths use SSE4.1
  intrinsics and won't compile without it; the CPU is still checked at
  runtime, and the code falls back to scalar without SSE4.1. MSVC needs no
  flag.
* **AVIF / AV1:** define `HEIC_HAVE_DAV1D`, put `dav1d/dav1d.h` on the include
  path, and link libdav1d. Without it, AV1 items fail to decode with "AV1
  decode requires dav1d (build with HEIC_HAVE_DAV1D)".
* **Compressed unci:** define `HEIC_HAVE_ZLIB` (zlib/deflate; `zlib.h`, link
  zlib) and/or `HEIC_HAVE_BROTLI` (`brotli/decode.h`, link brotlidec +
  brotlicommon). Without them, uncompressed unci still decodes and compressed
  unci fails with an error.

```
clang -std=c11 -O2 -msse4.1 -c dist/heic.c                    # HEVC + uncompressed unci
clang -std=c11 -O2 -msse4.1 -DHEIC_HAVE_DAV1D -c dist/heic.c  # + AVIF (link libdav1d)
```

### SumatraPDF surface

[SumatraPDF](https://www.sumatrapdfreader.org/) uses this library as its
HEIC/AVIF reader (`sumatrapdf/src/shared/AvifReader.cpp`):

| Need | API |
|------|-----|
| size from buffer | `heic_doc_open` + `heic_doc_info` |
| decode to BGRA pixmap | `heic_doc_decode(HEIC_FORMAT_BGRA)` |
| EXIF TIFF blob | `heic_doc_exif` (HEIF offset header already skipped) |

Smoke that path with `heic_test -sumatra file.heic` (or `bun cmd/verify-release.ts`).

### Supported profiles and intentional exclusions

| In scope | Notes |
|----------|--------|
| HEVC stills (`hvc1`/`heic`/`heix`/`mif1`) | I-frames, grids, tiles, WPP (entry points), P-item predictives |
| AV1 stills (`av01`/`avif`) | via **dav1d** (`HEIC_HAVE_DAV1D` + link dav1d; not in amalgamation/WASM); layered AVIF decodes the top layer only |
| unci (ISO 23001-17) | planar/pixel/row/tile; zlib/deflate with `HEIC_HAVE_ZLIB`, brotli with `HEIC_HAVE_BROTLI` |
| Alpha | `auxl` HEVC/AVIF; AVIF alpha sequences |
| EXIF / XMP / ICC / clap / irot / imir / iovl | EXIF returns TIFF only; an Exif item without the HEIF offset header (raw TIFF) reports `has_exif=1` but `heic_doc_exif` returns 0 |
| HEVC/AV1 image sequences | `moov` + public sequence API |
| Apple HDR gain map | independent aux decode; **no** tone-map/EOTF |

| Out of scope / limited | Notes |
|------------------------|--------|
| Encode / write HEIF | decode-only |
| Multilayer HEVC (inter-layer) | not supported: open/info succeed, decode fails (e.g. "unsupported PPS extension") |
| JPEG / H.264 in HEIF | not supported: open/info succeed, decode fails with "unsupported item type" |
| PQ/HLG tone-mapping | CICP exposed; caller tone-maps |
| GPU backends | CPU only |
| DELTAQP_A / CCP residual polish | remaining FATE pin gaps (see `PROGRESS.md`) |
| Negative-offset `iovl` vs libheif | ISO signed offsets (current libheif agrees; older libheif painted black) |

## Build & test

Requires `clang` or MSVC, `bun`, `git`, `cmake`, and `ninja` (every
`cmd/build.ts` run builds static zlib and brotli with cmake/ninja if they are
missing). For AVIF, also `meson` (to build static dav1d).

```
bun cmd/get-deps.ts              # clone deps + download/regenerate deps/testimages
bun cmd/build.ts                 # HEVC harness (+ zlib/brotli, built if missing)
bun cmd/build.ts -dav1d          # also link dav1d (auto-builds if missing)
bun cmd/build.ts -clang -dav1d
bun cmd/tests.ts -all            # decode all corpus files; RGB mse vs libheif
bun cmd/tests.ts -info -all      # open/probe only
bun cmd/build.ts -libheif        # also link strukturag libheif oracle
bun cmd/bench.ts -rand 5         # compact best-of-3 timing vs libheif
bun cmd/bench.ts -verbose -rand 5 # also show open/decode/close timing
bun cmd/fuzz.ts                  # libFuzzer + ASan (seeded from deps corpus)
bun cmd/fuzz.ts -check-crashes   # replay fuzz/crashes/* under ASan (CI)
bun cmd/fuzz-afl.ts              # AFL++ on macOS (shares fuzz/corpus/)
bun cmd/build-dist.ts            # amalgamation → dist/ (+ split JS/WASM demo)
bun cmd/build-wasm.ts            # WebAssembly drop only (bootstraps emsdk if needed)
bun cmd/run-wasm-demo.ts         # serve dist/wasm/demo.html on localhost:8000
bun cmd/verify-wasm.ts single.heic
bun cmd/verify-release.ts        # Sumatra/API/memory-limit + amalgamation + WASM
```

Fuzzing notes: first run builds optional dav1d/zlib/brotli when missing, seeds
`fuzz/corpus/` from the corpus, then mutates until you Ctrl-C (rerun resumes).
Crashes land in `fuzz/crashes/` (commit them). Useful flags: `-jobs N`,
`-repro FILE`, `-minimize`, `-no-deps` (HEVC-only, skip codec builds).

On macOS you can also use AFL++ (`brew install afl++`, then once
`sudo afl-system-config`): `bun cmd/fuzz-afl.ts` builds the same
`LLVMFuzzerTestOneInput` harness with `afl-clang-fast` and shares
`fuzz/corpus/` with libFuzzer. AFL state is in `fuzz/afl-out/`; use
`-import` (also runs on exit) to merge the queue back into the shared corpus.

### WebAssembly demo

```
bun cmd/build-wasm.ts
bun cmd/run-wasm-demo.ts
```

`dist/wasm/heic.js` loads the separate `dist/wasm/heic.wasm` binary, so serve
the directory over HTTP rather than opening the demo through `file://`. The
browser drop is pure-C **HEVC + uncompressed unci** only: no AVIF (no dav1d)
and no zlib/brotli unci.

`heic_test` CLI:

```
heic_test -info in.heic
heic_test -out out.ppm in.heic
heic_test -rgba -out out.ppm in.heic   # decode RGBA (PPM still drops A)
heic_test -thumbnail -out thumb.ppm in.heic
heic_test -bench in.heic              # best-of-3 timing; + libheif with -libheif, + WIC on Windows
heic_test -verify in.heic             # RGB MSE vs libheif (build with -libheif)
heic_test -verify-gain-map in.heic    # gain map vs libheif aux decode (-libheif)
heic_test -verify-sequence in.heic    # every sequence frame vs libheif (-libheif)
heic_test -exif in.heic
heic_test -sumatra in.heic            # size + BGRA + EXIF (SumatraPDF surface)
heic_test -api in.heic                # custom allocator + abort
heic_test -memory-limit               # max_memory_bytes regression (no file)
heic_test -sequence-info in.heic      # sequence timing + per-frame info
heic_test -sequence-frame N [-rgba] [-out f.ppm] in.heic  # decode one sequence frame
heic_test -hevc-sequence [-hevc-frames N] [-out f.yuv] in.bit  # raw Annex B HEVC stream
heic_test -profile-heic N in.heic     # decode N times (winperf marks)
heic_test -profile-libheif N in.heic  # same for libheif (-libheif)
```

Oracle build (static, cmake/ninja, same idea as djvudec↔libdjvu):

```
bun cmd/build.ts ref             # dav1d + libde265 + zlib + brotli + libheif
bun cmd/build.ts -clang -libheif # harness linked with heif + libde265 + dav1d + zlib + brotli
```

## How it was made

AI-assisted port of [imazen/heic](https://github.com/imazen/heic) (Rust) to C,
in the style of [djvudec](https://github.com/kjk/djvudec). AV1 uses dav1d as-is.
Correctness is checked against libheif on the imazen/heic testdata corpus.

## Patents

HEVC may be covered by third-party patents. This project grants copyright
permissions only. See [LICENSE.md](LICENSE.md).
