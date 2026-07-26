// verify-wasm.ts — smoke-test dist/wasm/heic.js through the same
// exports the web app uses.
//
//   bun cmd/verify-wasm.ts <file.heic | -rand N>
//   bun cmd/verify-wasm.ts deps/heic/testdata/features/single.heic
import path from "node:path";
import { existsSync, readFileSync } from "node:fs";
import { getDeps } from "./get-deps";
import { corpusSummary, selectFiles } from "./corpus";
import { ensureWasm, WASM_JS } from "./build-wasm";

const ROOT = path.resolve(import.meta.dir, "..");
await getDeps();
const [file] = selectFiles(
  `usage: bun cmd/verify-wasm.ts <file.heic|.heif|.avif | -rand 1>

${corpusSummary()}`,
);

ensureWasm(false);
if (!existsSync(WASM_JS))
  throw new Error("dist/wasm/heic.js missing after build");

/* The glue is built for ENVIRONMENT=web; provide the browser globals it probes. */
(globalThis as any).self = globalThis;
const createHeicModule = (await import(WASM_JS)).default;
const M: any = await createHeicModule();

M._heic_init();
const ctx = M._heic_ctx_new(0, 0, 0, 0);
if (!ctx) throw new Error("heic_ctx_new failed");

const bytes = new Uint8Array(readFileSync(file));
const buf = M._malloc(bytes.length);
if (!buf) throw new Error("malloc failed");
M.HEAPU8.set(bytes, buf);

const doc = M._heic_doc_open(ctx, buf, bytes.length);
if (!doc) throw new Error("heic_doc_open failed");

/* heic_image_info layout (wasm32): width@0 height@4 bit_depth@8 … */
const INFO_BYTES = 48;
const info = M._malloc(INFO_BYTES);
if (M._heic_doc_info(doc, info) !== 0) throw new Error("heic_doc_info failed");
const iw = M.HEAPU32[info >> 2];
const ih = M.HEAPU32[(info >> 2) + 1];
const bd = M.HEAP32[(info >> 2) + 2];
const alpha = M.HEAP32[(info >> 2) + 3];
const kind = M._heic_doc_kind(doc);
console.log(
  `${path.basename(file)}: kind=${kind} ${iw}x${ih} bit_depth=${bd} alpha=${alpha}`,
);
M._free(info);

/* HEIC_FORMAT_RGB = 3 */
const img = M._heic_doc_decode(doc, 3);
if (!img) throw new Error("heic_doc_decode failed (AVIF needs dav1d; not in wasm drop)");
const w = M.HEAPU32[img >> 2];
const h = M.HEAPU32[(img >> 2) + 1];
const fmt = M.HEAP32[(img >> 2) + 2];
const stride = M.HEAP32[(img >> 2) + 3];
const data = M.HEAPU32[(img >> 2) + 4];

let min = 255,
  max = 0,
  sum = 0,
  n = 0;
for (let y = 0; y < h; y += Math.max(1, (h / 8) | 0)) {
  for (let x = 0; x < w; x += Math.max(1, (w / 8) | 0)) {
    const v = M.HEAPU8[data + y * stride + x * 3];
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
    n++;
  }
}
console.log(
  `decode: ${w}x${h} fmt=${fmt} stride=${stride} sampled[min=${min} max=${max} mean=${(sum / n).toFixed(1)}]`,
);
if (w !== iw || h !== ih) throw new Error("decode dims != info dims");
if (min === max && w * h > 16)
  throw new Error("uniform image — decode likely broken");
console.log("✓ wasm decode OK");

M._heic_image_destroy(ctx, img);
M._heic_doc_close(doc);
M._free(buf);
M._heic_ctx_free(ctx);
