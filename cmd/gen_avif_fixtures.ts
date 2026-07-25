// Build classic meta AVIF fixtures (grid + alpha) from single-item AVIFs.
//
//   bun cmd/gen_avif_fixtures.ts [outDir]
//
// No AV1 encoder: reuses sample OBUs + hand-rolled HEIF (ftyp/meta/mdat).
// Default outDir: deps/testimages/avif (created by get-deps.ts).
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "fs";
import { join } from "path";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const DEFAULT_OUT = join(ROOT, "deps", "testimages", "avif");

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
  const t = new TextEncoder().encode(typ);
  if (t.length !== 4) throw new Error(`bad box type ${typ}`);
  return concat(be32(8 + payload.length), t, payload);
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
function rb32(d: Uint8Array, o: number): number {
  return ((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0;
}
function rb16(d: Uint8Array, o: number): number {
  return (d[o] << 8) | d[o + 1];
}
function fourcc(d: Uint8Array, o: number): string {
  return String.fromCharCode(d[o], d[o + 1], d[o + 2], d[o + 3]);
}

export type Av01Extract = {
  av1c: Uint8Array;
  sample: Uint8Array;
  w: number;
  h: number;
};

/** Pull av1C + mdat sample + ispe size from a simple single-item AVIF. */
export function extractAv01(path: string): Av01Extract {
  const data = new Uint8Array(readFileSync(path));
  let pos = 0;
  let av1c: Uint8Array | null = null;
  let ispe: [number, number] | null = null;
  let mdat: Uint8Array | null = null;
  const end = data.length;
  while (pos + 8 <= end) {
    const size = rb32(data, pos);
    const typ = fourcc(data, pos + 4);
    if (size < 8 || pos + size > end) break;
    const body = data.subarray(pos + 8, pos + size);
    if (typ === "mdat") mdat = body.slice();
    else if (typ === "meta") {
      let cpos = 4;
      while (cpos + 8 <= body.length) {
        const csz = rb32(body, cpos);
        const ctyp = fourcc(body, cpos + 4);
        if (csz < 8 || cpos + csz > body.length) break;
        const cbody = body.subarray(cpos + 8, cpos + csz);
        if (ctyp === "iprp") {
          let ip = 0;
          while (ip + 8 <= cbody.length) {
            const isz = rb32(cbody, ip);
            const ityp = fourcc(cbody, ip + 4);
            if (isz < 8 || ip + isz > cbody.length) break;
            const ibody = cbody.subarray(ip + 8, ip + isz);
            if (ityp === "ipco") {
              let p = 0;
              while (p + 8 <= ibody.length) {
                const psz = rb32(ibody, p);
                const ptyp = fourcc(ibody, p + 4);
                if (psz < 8 || p + psz > ibody.length) break;
                const pbody = ibody.subarray(p + 8, p + psz);
                if (ptyp === "av1C") av1c = pbody.slice();
                else if (ptyp === "ispe" && pbody.length >= 12)
                  ispe = [rb32(pbody, 4), rb32(pbody, 8)];
                p += psz;
              }
            }
            ip += isz;
          }
        }
        cpos += csz;
      }
    }
    pos += size;
  }
  if (!av1c || !mdat || !ispe)
    throw new Error(`failed to extract av01 from ${path}`);
  return { av1c, sample: mdat, w: ispe[0], h: ispe[1] };
}

function ispeBox(w: number, h: number): Uint8Array {
  return fullbox("ispe", 0, 0, concat(be32(w), be32(h)));
}
function av1cBox(av1c: Uint8Array): Uint8Array {
  return box("av1C", av1c);
}
function colrNclx(
  prim = 1,
  transfer = 13,
  matrix = 6,
  full = false,
): Uint8Array {
  return box(
    "colr",
    concat(
      new TextEncoder().encode("nclx"),
      be16(prim),
      be16(transfer),
      be16(matrix),
      new Uint8Array([full ? 0x80 : 0]),
    ),
  );
}
function pixiBox(depth = 8, n = 3): Uint8Array {
  return fullbox("pixi", 0, 0, new Uint8Array([n, ...Array(n).fill(depth)]));
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
function ilocExtents(
  items: { id: number; off: number; len: number }[],
): Uint8Array {
  const parts: Uint8Array[] = [new Uint8Array([0x44, 0x00]), be16(items.length)];
  for (const it of items) {
    parts.push(
      be16(it.id),
      be16(0),
      be16(0),
      be16(1),
      be32(it.off),
      be32(it.len),
    );
  }
  return fullbox("iloc", 1, 0, concat(...parts));
}
function ipmaEntries(
  entries: { id: number; props: number[] }[],
): Uint8Array {
  const parts: Uint8Array[] = [be32(entries.length)];
  for (const e of entries) {
    parts.push(be16(e.id), new Uint8Array([e.props.length]));
    for (const p of e.props) parts.push(new Uint8Array([p & 0xff]));
  }
  return fullbox("ipma", 0, 0, concat(...parts));
}
function irefDimg(fromId: number, toIds: number[]): Uint8Array {
  const parts: Uint8Array[] = [be16(fromId), be16(toIds.length)];
  for (const t of toIds) parts.push(be16(t));
  return fullbox("iref", 0, 0, box("dimg", concat(...parts)));
}
function irefAuxl(fromId: number, toId: number): Uint8Array {
  return fullbox(
    "iref",
    0,
    0,
    box("auxl", concat(be16(fromId), be16(1), be16(toId))),
  );
}
function auxcBox(urn: string): Uint8Array {
  return fullbox(
    "auxC",
    0,
    0,
    concat(new TextEncoder().encode(urn), new Uint8Array([0])),
  );
}
function gridPayload(
  rows: number,
  cols: number,
  outW: number,
  outH: number,
): Uint8Array {
  return concat(
    new Uint8Array([0, 1, rows - 1, cols - 1]),
    be32(outW),
    be32(outH),
  );
}

export function writeGrid(
  color: Av01Extract,
  path: string,
  rows = 2,
  cols = 2,
): void {
  const tw = color.w,
    th = color.h;
  const outW = tw * cols,
    outH = th * rows;
  const nTiles = rows * cols;
  const tileIds = Array.from({ length: nTiles }, (_, i) => 2 + i);
  const gridData = gridPayload(rows, cols, outW, outH);
  const sample = color.sample;
  const mdatBytes = concat(gridData, sample);

  const ipco = box(
    "ipco",
    concat(
      ispeBox(outW, outH),
      ispeBox(tw, th),
      av1cBox(color.av1c),
      colrNclx(),
      pixiBox(8, 3),
    ),
  );
  const ipmaList: { id: number; props: number[] }[] = [{ id: 1, props: [1] }];
  for (const tid of tileIds)
    ipmaList.push({ id: tid, props: [0x82, 0x83, 4, 5] });
  const ipma = ipmaEntries(ipmaList);
  const iprp = box("iprp", concat(ipco, ipma));

  let infes = infe(1, "grid");
  for (const tid of tileIds) infes = concat(infes, infe(tid, "av01"));
  const iinf = fullbox("iinf", 0, 0, concat(be16(1 + nTiles), infes));
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
  const iref = irefDimg(1, tileIds);
  const ftyp = box(
    "ftyp",
    concat(
      new TextEncoder().encode("avif"),
      be32(0),
      new TextEncoder().encode("avif"),
      new TextEncoder().encode("mif1"),
      new TextEncoder().encode("miaf"),
    ),
  );
  const metaNoIloc = concat(hdlr, pitm, iinf, iprp, iref);
  let dummy = ilocExtents([
    { id: 1, off: 0, len: gridData.length },
    ...tileIds.map((t) => ({ id: t, off: 0, len: sample.length })),
  ]);
  let meta = fullbox("meta", 0, 0, concat(metaNoIloc, dummy));
  for (let i = 0; i < 3; i++) {
    const mdatFileOff = ftyp.length + meta.length;
    const gridAbs = mdatFileOff + 8;
    const sampleAbs = gridAbs + gridData.length;
    dummy = ilocExtents([
      { id: 1, off: gridAbs, len: gridData.length },
      ...tileIds.map((t) => ({ id: t, off: sampleAbs, len: sample.length })),
    ]);
    meta = fullbox("meta", 0, 0, concat(metaNoIloc, dummy));
  }
  const out = concat(ftyp, meta, box("mdat", mdatBytes));
  writeFileSync(path, out);
  console.log(
    `wrote ${path} (${out.length} bytes) grid ${cols}x${rows} ${outW}x${outH}`,
  );
}

export function writeAlpha(
  color: Av01Extract,
  alpha: Av01Extract,
  path: string,
): void {
  if (color.w !== alpha.w || color.h !== alpha.h)
    throw new Error("color/alpha size mismatch");
  const w = color.w,
    h = color.h;
  const mdatBytes = concat(color.sample, alpha.sample);
  const ipco = box(
    "ipco",
    concat(
      ispeBox(w, h),
      av1cBox(color.av1c),
      colrNclx(),
      pixiBox(8, 3),
      av1cBox(alpha.av1c),
      pixiBox(8, 1),
      auxcBox("urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"),
    ),
  );
  const ipma = ipmaEntries([
    { id: 1, props: [0x81, 0x82, 3, 4] },
    { id: 2, props: [0x81, 0x85, 6, 7] },
  ]);
  const iprp = box("iprp", concat(ipco, ipma));
  const iinf = fullbox(
    "iinf",
    0,
    0,
    concat(be16(2), infe(1, "av01"), infe(2, "av01")),
  );
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
  const iref = irefAuxl(2, 1);
  const ftyp = box(
    "ftyp",
    concat(
      new TextEncoder().encode("avif"),
      be32(0),
      new TextEncoder().encode("avif"),
      new TextEncoder().encode("mif1"),
      new TextEncoder().encode("miaf"),
    ),
  );
  const metaNo = concat(hdlr, pitm, iinf, iprp, iref);
  let dummy = ilocExtents([
    { id: 1, off: 0, len: color.sample.length },
    { id: 2, off: 0, len: alpha.sample.length },
  ]);
  let meta = fullbox("meta", 0, 0, concat(metaNo, dummy));
  for (let i = 0; i < 3; i++) {
    const mdatOff = ftyp.length + meta.length + 8;
    dummy = ilocExtents([
      { id: 1, off: mdatOff, len: color.sample.length },
      {
        id: 2,
        off: mdatOff + color.sample.length,
        len: alpha.sample.length,
      },
    ]);
    meta = fullbox("meta", 0, 0, concat(metaNo, dummy));
  }
  const out = concat(ftyp, meta, box("mdat", mdatBytes));
  writeFileSync(path, out);
  console.log(`wrote ${path} (${out.length} bytes) alpha ${w}x${h}`);
}

/** Generate grid_2x2.avif + alpha.avif into outDir (expects fox stills present). */
export function generateAvifFixtures(outDir: string = DEFAULT_OUT): void {
  mkdirSync(outDir, { recursive: true });
  const colorPath = join(outDir, "fox.profile0.8bpc.yuv420.avif");
  let monoPath = join(outDir, "_src", "fox.profile0.8bpc.yuv420.monochrome.avif");
  if (!existsSync(monoPath))
    monoPath = join(outDir, "fox.profile0.8bpc.yuv420.monochrome.avif");
  if (!existsSync(colorPath))
    throw new Error(`missing ${colorPath} (download via get-deps first)`);
  const color = extractAv01(colorPath);
  writeGrid(color, join(outDir, "grid_2x2.avif"), 2, 2);
  if (existsSync(monoPath)) {
    const mono = extractAv01(monoPath);
    writeAlpha(color, mono, join(outDir, "alpha.avif"));
  }
}

if (import.meta.main) {
  const out = process.argv[2] ?? DEFAULT_OUT;
  generateAvifFixtures(out);
}
