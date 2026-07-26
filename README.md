# heic — a plain-C HEIC/HEIF/AVIF decoder

Decode-only library for HEIC/HEIF/AVIF still images, aimed at easy embedding
(e.g. [SumatraPDF](https://www.sumatrapdfreader.org/)):

* plain C, jbig2dec-style API
* **HEVC**: pure-C port of [imazen/heic](https://github.com/imazen/heic)
* **AV1**: [videolan dav1d](https://code.videolan.org/videolan/dav1d) (C; linked, not re-ported)
* HEVC image sequences: probe/decode the first sync frame
* simple drop-in amalgamation: `dist/heic.h` + `dist/heic.c` (+ link dav1d for AVIF)
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

## Build & test

Requires `clang` or MSVC, `bun`, and `git`. For AVIF, also `meson` + `ninja`
(to build static dav1d).

```
bun cmd/get-deps.ts              # clone deps + download/regenerate deps/testimages
bun cmd/build.ts                 # HEVC harness (+ zlib/brotli for unci if present)
bun cmd/build.ts -dav1d          # also link dav1d (auto-builds if missing)
bun cmd/build.ts -clang -dav1d
bun cmd/tests.ts -all            # decode all corpus files; RGB mse vs libheif
bun cmd/tests.ts -info -all      # open/probe only
bun cmd/build.ts -libheif        # also link strukturag libheif oracle
bun cmd/bench.ts -rand 5         # compact best-of-3 timing vs libheif
bun cmd/bench.ts -verbose -rand 5 # also show open/decode/close timing
bun cmd/fuzz.ts                  # libFuzzer + ASan (seeded from deps corpus)
bun cmd/build-dist.ts            # amalgamation → dist/ (+ split JS/WASM demo)
bun cmd/build-wasm.ts            # WebAssembly drop only (bootstraps emsdk if needed)
bun cmd/run-wasm-demo.ts         # serve dist/wasm/demo.html on localhost:8000
bun cmd/verify-wasm.ts single.heic
```

Fuzzing notes: first run builds optional dav1d/zlib/brotli when missing, seeds
`fuzz/corpus/` from the corpus, then mutates until you Ctrl-C (rerun resumes).
Crashes land in `fuzz/crashes/` (commit them). Useful flags: `-jobs N`,
`-repro FILE`, `-minimize`, `-no-deps` (HEVC-only, skip codec builds).

### WebAssembly demo

```
bun cmd/build-wasm.ts
bun cmd/run-wasm-demo.ts
```

`dist/wasm/heic.js` loads the separate `dist/wasm/heic.wasm` binary, so serve
the directory over HTTP rather than opening the demo through `file://`. The
browser drop is pure-C **HEVC + unci** (no dav1d); AVIF needs dav1d linked by
the host, same as the amalgamation.

`heic_test` CLI:

```
heic_test -info in.heic
heic_test -out out.ppm in.heic
heic_test -rgba -out out.ppm in.heic   # decode RGBA (PPM still drops A)
heic_test -thumbnail -out thumb.ppm in.heic
heic_test -bench in.heic              # vs libheif (build with -libheif)
heic_test -verify in.heic             # RGB MSE vs libheif (build with -libheif)
heic_test -exif in.heic
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
