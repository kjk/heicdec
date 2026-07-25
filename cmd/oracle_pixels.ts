// Compare our PPM (RGB) against a reference PPM (or note pillow_heif gap).
//
//   bun cmd/oracle_pixels.ts --ppm-only our.ppm ref.ppm
//   bun cmd/oracle_pixels.ts file.heic our.ppm   # needs pillow_heif via python
//
// Native path uses libheif via heic_test -verify; this helper is for PPM diffs.
// Exit 0 if mse <= threshold (default 4.0), else 1.
import { readFileSync } from "fs";
import { spawnSync } from "child_process";

function readPpm(path: string): { w: number; h: number; raw: Uint8Array } {
  const data = new Uint8Array(readFileSync(path));
  if (data[0] !== 0x50 || data[1] !== 0x36)
    throw new Error(`${path}: not a binary P6 PPM`);
  let i = 2;
  const isWs = (c: number) => c === 32 || c === 9 || c === 13 || c === 10;
  const skip = () => {
    while (i < data.length && isWs(data[i]!)) i++;
    while (i < data.length && data[i] === 0x23) {
      while (i < data.length && data[i] !== 10 && data[i] !== 13) i++;
      while (i < data.length && isWs(data[i]!)) i++;
    }
  };
  const token = (): string => {
    skip();
    const j = i;
    while (i < data.length && !isWs(data[i]!) && data[i] !== 0x23) i++;
    return new TextDecoder().decode(data.subarray(j, i));
  };
  const w = parseInt(token(), 10);
  const h = parseInt(token(), 10);
  const maxv = parseInt(token(), 10);
  if (maxv !== 255) throw new Error(`${path}: maxval ${maxv} not supported`);
  while (i < data.length && (data[i] === 32 || data[i] === 9 || data[i] === 13))
    i++;
  if (i < data.length && data[i] === 10) i++;
  const need = w * h * 3;
  const raw = data.subarray(i, i + need);
  if (raw.length < need) throw new Error(`${path}: truncated pixel data`);
  return { w, h, raw };
}

function compare(
  a: Uint8Array,
  b: Uint8Array,
  w: number,
  h: number,
): { mse: number; maxd: number; ndiff: number } {
  const n = w * h * 3;
  let sse = 0,
    maxd = 0,
    ndiff = 0;
  for (let i = 0; i < n; i++) {
    const d = Math.abs(a[i]! - b[i]!);
    if (d) {
      ndiff++;
      sse += d * d;
      if (d > maxd) maxd = d;
    }
  }
  return { mse: sse / n, maxd, ndiff };
}

function refViaPython(heicPath: string): { w: number; h: number; raw: Uint8Array } {
  /* Optional: pillow_heif if installed; prefer heic_test -verify for oracle. */
  const py = `
from pathlib import Path
import sys
try:
    from pillow_heif import open_heif
    import numpy as np
except ImportError:
    sys.stderr.write("pillow_heif/numpy not installed; use --ppm-only or heic_test -verify\\n")
    sys.exit(3)
h = open_heif(sys.argv[1])
img = h.to_pillow()
if img.mode in ("I;16", "I;16L", "I;16B", "I") or img.mode.startswith("I;"):
    g = np.asarray(img, dtype=np.uint32)
    g8 = (g >> 8).astype(np.uint8) if g.max() > 255 else g.astype(np.uint8)
    rgb = np.stack([g8, g8, g8], axis=-1)
    ht, w = rgb.shape[:2]
    sys.stdout.buffer.write(f"{w} {ht}\\n".encode())
    sys.stdout.buffer.write(rgb.tobytes())
else:
    rgb = img.convert("RGB")
    w, ht = rgb.size
    sys.stdout.buffer.write(f"{w} {ht}\\n".encode())
    sys.stdout.buffer.write(rgb.tobytes())
`;
  const r = spawnSync("python", ["-c", py, heicPath], {
    encoding: "buffer",
    maxBuffer: 256 * 1024 * 1024,
  });
  if (r.status !== 0) {
    process.stderr.write(r.stderr ?? Buffer.from(""));
    process.exit(r.status ?? 3);
  }
  const out = new Uint8Array(r.stdout!);
  let i = 0;
  while (i < out.length && out[i] !== 10) i++;
  const [ws, hs] = new TextDecoder().decode(out.subarray(0, i)).split(" ");
  const w = parseInt(ws!, 10),
    h = parseInt(hs!, 10);
  return { w, h, raw: out.subarray(i + 1) };
}

const argv = process.argv.slice(2);
let ppmOnly = false;
let threshold = 4.0;
const pos: string[] = [];
for (let i = 0; i < argv.length; i++) {
  const a = argv[i]!;
  if (a === "--ppm-only") ppmOnly = true;
  else if (a === "--threshold") threshold = parseFloat(argv[++i] ?? "4");
  else if (!a.startsWith("-")) pos.push(a);
}
if (pos.length < 2) {
  console.error(
    "usage: bun cmd/oracle_pixels.ts [--ppm-only] [--threshold N] file_or_ppm our.ppm",
  );
  process.exit(2);
}
const our = readPpm(pos[1]!);
let ref: { w: number; h: number; raw: Uint8Array };
if (ppmOnly) ref = readPpm(pos[0]!);
else ref = refViaPython(pos[0]!);

if (our.w !== ref.w || our.h !== ref.h) {
  console.error(
    `size mismatch ours=${our.w}x${our.h} ref=${ref.w}x${ref.h}`,
  );
  process.exit(2);
}
const { mse, maxd, ndiff } = compare(our.raw, ref.raw, our.w, our.h);
console.log(
  `${our.w}x${our.h} mse=${mse.toFixed(4)} maxdiff=${maxd} n_diff=${ndiff}`,
);
process.exit(mse <= threshold ? 0 : 1);
