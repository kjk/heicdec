// Generate tiny unci HEIFs with block_size≠0 for regression.
//
//   bun cmd/gen_unci_block_fixtures.ts [outDir]
//
// Default outDir: deps/testimages/unci_block
import { mkdirSync, writeFileSync } from "fs";
import { join } from "path";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const DEFAULT_OUT = join(ROOT, "deps", "testimages", "unci_block");

function be16(n: number): Uint8Array {
  const b = new Uint8Array(2);
  b[0] = (n >>> 8) & 0xff;
  b[1] = n & 0xff;
  return b;
}
function be32(n: number): Uint8Array {
  const b = new Uint8Array(4);
  b[0] = (n >>> 24) & 0xff;
  b[1] = (n >>> 16) & 0xff;
  b[2] = (n >>> 8) & 0xff;
  b[3] = n & 0xff;
  return b;
}
function concat(...parts: Uint8Array[]): Uint8Array {
  let n = 0;
  for (const p of parts) n += p.length;
  const out = new Uint8Array(n);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}
function box(typ: string, payload: Uint8Array): Uint8Array {
  return concat(
    be32(8 + payload.length),
    new TextEncoder().encode(typ),
    payload,
  );
}
function fullbox(
  typ: string,
  version: number,
  flags: number,
  payload: Uint8Array,
): Uint8Array {
  return box(
    typ,
    concat(
      new Uint8Array([
        version,
        (flags >>> 16) & 0xff,
        (flags >>> 8) & 0xff,
        flags & 0xff,
      ]),
      payload,
    ),
  );
}
function ispe(w: number, h: number): Uint8Array {
  return fullbox("ispe", 0, 0, concat(be32(w), be32(h)));
}
function unccBox(opts: {
  ncomp: number;
  depths: number[];
  sampling: number;
  interleave: number;
  blockSize: number;
  flags: number;
  pixelSize: number;
}): Uint8Array {
  const body: Uint8Array[] = [be32(0), be32(opts.ncomp)];
  for (let i = 0; i < opts.ncomp; i++) {
    body.push(be16(i), new Uint8Array([opts.depths[i]! - 1, 0, 0]));
  }
  body.push(
    new Uint8Array([
      opts.sampling,
      opts.interleave,
      opts.blockSize,
      opts.flags,
    ]),
    be32(opts.pixelSize),
    be32(0),
    be32(0),
    be32(0),
    be32(0),
  );
  return fullbox("uncC", 0, 0, concat(...body));
}
function cmpdBox(types: number[]): Uint8Array {
  const parts: Uint8Array[] = [be32(types.length)];
  for (const t of types) parts.push(be16(t));
  return box("cmpd", concat(...parts));
}
function infe(itemId: number, itemType: string): Uint8Array {
  return fullbox(
    "infe",
    2,
    0,
    concat(
      be16(itemId),
      be16(0),
      new TextEncoder().encode(itemType),
      new Uint8Array([0]),
    ),
  );
}
function ilocOne(itemId: number, offset: number, length: number): Uint8Array {
  return fullbox(
    "iloc",
    1,
    0,
    concat(
      new Uint8Array([0x44, 0x00]),
      be16(1),
      be16(itemId),
      be16(0),
      be16(0),
      be16(1),
      be32(offset),
      be32(length),
    ),
  );
}
function ipmaOne(itemId: number, props: number[]): Uint8Array {
  return fullbox(
    "ipma",
    0,
    0,
    concat(
      be32(1),
      be16(itemId),
      new Uint8Array([props.length]),
      ...props.map((p) => new Uint8Array([p & 0xff])),
    ),
  );
}
function writeHeif(
  path: string,
  uncc: Uint8Array,
  cmpd: Uint8Array | null,
  sample: Uint8Array,
  w: number,
  h: number,
): void {
  let ipcoParts = concat(ispe(w, h), uncc);
  const props = [0x81, 0x82];
  if (cmpd) {
    ipcoParts = concat(ipcoParts, cmpd);
    props.push(3);
  }
  const ipco = box("ipco", ipcoParts);
  const ipma = ipmaOne(1, props);
  const iprp = box("iprp", concat(ipco, ipma));
  const iinf = fullbox("iinf", 0, 0, concat(be16(1), infe(1, "unci")));
  const pitm = fullbox("pitm", 0, 0, be16(1));
  const hdlr = fullbox(
    "hdlr",
    0,
    0,
    concat(
      be32(0),
      new TextEncoder().encode("pict"),
      be32(0),
      be32(0),
      be32(0),
      new Uint8Array([0]),
    ),
  );
  const ftyp = box(
    "ftyp",
    concat(
      new TextEncoder().encode("mif1"),
      be32(0),
      new TextEncoder().encode("mif1"),
      new TextEncoder().encode("heic"),
      new TextEncoder().encode("unci"),
    ),
  );
  const metaNo = concat(hdlr, pitm, iinf, iprp);
  let dummy = ilocOne(1, 0, sample.length);
  let meta = fullbox("meta", 0, 0, concat(metaNo, dummy));
  for (let i = 0; i < 3; i++) {
    const absOff = ftyp.length + meta.length + 8;
    dummy = ilocOne(1, absOff, sample.length);
    meta = fullbox("meta", 0, 0, concat(metaNo, dummy));
  }
  const out = concat(ftyp, meta, box("mdat", sample));
  writeFileSync(path, out);
  console.log(`wrote ${path} (${out.length} bytes)`);
}
function packLe(val: number, nbytes: number): Uint8Array {
  const b = new Uint8Array(nbytes);
  let v = val >>> 0;
  for (let i = 0; i < nbytes; i++) {
    b[i] = v & 0xff;
    v >>>= 8;
  }
  return b;
}
function packBe(val: number, nbytes: number): Uint8Array {
  const b = new Uint8Array(nbytes);
  for (let i = 0; i < nbytes; i++)
    b[i] = (val >>> ((nbytes - 1 - i) * 8)) & 0xff;
  return b;
}

export function generateUnciBlockFixtures(outDir: string = DEFAULT_OUT): void {
  mkdirSync(outDir, { recursive: true });
  const w = 4,
    h = 4;

  const sample: number[] = [];
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const r = (x * 40 + y * 10) & 0x3ff;
      const g = (x * 20 + y * 30) & 0x3ff;
      const b = (x * 15 + y * 5) & 0x3ff;
      const word = (r << 20) | (g << 10) | b;
      sample.push(...packBe(word, 4));
    }
  }
  writeHeif(
    join(outDir, "rgb10_block_pixel_be.heif"),
    unccBox({
      ncomp: 3,
      depths: [10, 10, 10],
      sampling: 0,
      interleave: 1,
      blockSize: 4,
      flags: 0,
      pixelSize: 4,
    }),
    cmpdBox([4, 5, 6]),
    new Uint8Array(sample),
    w,
    h,
  );

  const sample2: number[] = [];
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const v = (x * 50 + y * 80) & 0x3ff;
      sample2.push(...packLe(v << 6, 2));
    }
  }
  writeHeif(
    join(outDir, "mono10_block_comp_le.heif"),
    unccBox({
      ncomp: 1,
      depths: [10],
      sampling: 0,
      interleave: 0,
      blockSize: 2,
      flags: 0x40 | 0x20,
      pixelSize: 0,
    }),
    cmpdBox([0]),
    new Uint8Array(sample2),
    w,
    h,
  );

  const sample3: number[] = [];
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const r = (x * 40) & 255,
        g = (y * 50) & 255,
        b = ((x + y) * 30) & 255;
      sample3.push(...packLe(r | (g << 8) | (b << 16), 3));
    }
  }
  writeHeif(
    join(outDir, "rgb8_block_pixel_le.heif"),
    unccBox({
      ncomp: 3,
      depths: [8, 8, 8],
      sampling: 0,
      interleave: 1,
      blockSize: 3,
      flags: 0x40 | 0x20,
      pixelSize: 3,
    }),
    cmpdBox([4, 5, 6]),
    new Uint8Array(sample3),
    w,
    h,
  );
}

if (import.meta.main) {
  generateUnciBlockFixtures(process.argv[2] ?? DEFAULT_OUT);
}
