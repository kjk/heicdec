// Source video for the checked-in HM streams (see README.md): a textured
// pattern drifting by fractional amounts, so inter prediction needs
// sub-sample interpolation and bi-prediction.
//
//   bun gen_yuv.ts hbd16 out.yuv W H FRAMES
//       16-bit 4:4:4 planar, little endian, bright (values near the top of
//       the range) with a brightness fade
//   bun gen_yuv.ts main8 out.yuv W H FRAMES
//       8-bit 4:2:0 planar
import { writeFileSync } from "node:fs";

const [kind, out, ws, hs, fs] = process.argv.slice(2);
const w = Number(ws);
const h = Number(hs);
const frames = Number(fs);
if (
  (kind !== "hbd16" && kind !== "main8") || !out
  || !(w > 0) || !(h > 0) || !(frames > 0)
) {
  console.error("usage: bun gen_yuv.ts hbd16|main8 out.yuv W H FRAMES");
  process.exit(2);
}

/** The pattern at luma position (u, v) for a plane: [smooth wave, checker]. */
function pattern(u: number, v: number, plane: number): [number, number] {
  const s = (Math.sin(u * 0.37 + plane) + Math.sin(v * 0.29 - plane)
    + Math.sin((u + v) * 0.11)) / 3.0;
  const edge = (Math.trunc(u / 9) + Math.trunc(v / 7)) % 2 ? 1.0 : 0.0;
  return [s, edge];
}

const clamp = (v: number, max: number): number =>
  Math.max(0, Math.min(max, Math.trunc(v)));

const wide = kind === "hbd16";
const sub = wide ? 1 : 2; // chroma subsampling in both directions
const bytesPerSample = wide ? 2 : 1;
const frameSamples = w * h + 2 * (w / sub) * (h / sub);
const buf = new Uint8Array(frames * frameSamples * bytesPerSample);
let at = 0;

for (let t = 0; t < frames; t++) {
  const dx = t * 1.75;
  const dy = t * 0.75;
  const gain = 1.0 - 0.04 * t;
  for (let plane = 0; plane < 3; plane++) {
    const scale = plane === 0 ? 1 : sub;
    const pw = w / scale;
    const ph = h / scale;
    for (let y = 0; y < ph; y++) {
      for (let x = 0; x < pw; x++) {
        const [s, edge] = pattern(x * scale + dx, y * scale + dy, plane);
        if (wide) {
          const val = clamp((36000 + 22000 * s + 7000 * edge) * gain, 65535);
          buf[at++] = val & 0xff;
          buf[at++] = val >> 8;
        } else {
          buf[at++] = clamp(128 + 80 * s + 25 * edge, 255);
        }
      }
    }
  }
}
writeFileSync(out, buf);
console.log(`${out}: ${frames} frames, ${buf.length} bytes`);
