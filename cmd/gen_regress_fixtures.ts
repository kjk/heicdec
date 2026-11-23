// gen_regress_fixtures.ts -- HEIF files that reproduce container-level bugs
// we fixed, built from one plain single-image HEIC of the corpus so that no
// binaries are checked in.
//
//   bun cmd/gen_regress_fixtures.ts [source.heic] [outDir]
//
// Written by get-deps.ts into deps/testimages/regress:
//   two_pps_grid.heic        2x1 grid whose hvcC carries PPS id 0 and 1; the
//                            slices use id 0. The first tile misses the
//                            parameter-set cache and the second hits it, and
//                            those two paths used to pick different PPS.
//   two_ipma_grid.heic       the same grid with its property associations in
//                            two ipma boxes; the second used to replace the
//                            first.
//   iovl_red_fill.heic       overlay whose canvas is filled with opaque red
//                            around the image; the fill colour used to be
//                            written as luma only.
//   essential_unknown.heic   the image with an unknown essential property;
//                            must be rejected.
//   _info/grid_no_ispe.heic  the grid without an ispe property on the grid item;
//                            its size must come from the grid descriptor.
//   _info/aux_no_auxc.heic   the image plus a copy referenced as `auxl` with
//                            no auxC property. It used to be reported (and
//                            decoded) as alpha and as an HDR gain map.
//                            libheif rejects the file, so it sits in a
//                            `_` directory the corpus walk skips; tests.ts
//                            checks it directly.
import { mkdirSync, readFileSync, writeFileSync } from "fs";
import { dirname, join } from "path";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const DEFAULT_SOURCE = join(ROOT, "deps/heic/testdata/features/single.heic");
const DEFAULT_OUT = join(ROOT, "deps/testimages/regress");

function be16(n: number): Uint8Array {
  return new Uint8Array([(n >>> 8) & 0xff, n & 0xff]);
}
function be32(n: number): Uint8Array {
  return new Uint8Array([(n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff]);
}
function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let pos = 0;
  for (const part of parts) {
    out.set(part, pos);
    pos += part.length;
  }
  return out;
}
const ascii = (s: string) => new TextEncoder().encode(s);
function box(type: string, payload: Uint8Array): Uint8Array {
  return concat(be32(payload.length + 8), ascii(type), payload);
}
function fullbox(type: string, version: number, flags: number, payload: Uint8Array): Uint8Array {
  return box(type, concat(be32(((version << 24) | flags) >>> 0), payload));
}
const rb16 = (d: Uint8Array, o: number) => (d[o]! << 8) | d[o + 1]!;
const rb32 = (d: Uint8Array, o: number) =>
  ((d[o]! << 24) | (d[o + 1]! << 16) | (d[o + 2]! << 8) | d[o + 3]!) >>> 0;
function rbN(d: Uint8Array, o: number, n: number): number {
  let v = 0;
  for (let i = 0; i < n; i++) v = v * 256 + d[o + i]!;
  return v;
}

type Box = { type: string; start: number; body: number; end: number };

function* boxes(d: Uint8Array, start: number, end: number): Generator<Box> {
  let pos = start;
  while (pos + 8 <= end) {
    const size = rb32(d, pos);
    if (size < 8 || pos + size > end) return;
    yield {
      type: String.fromCharCode(d[pos + 4]!, d[pos + 5]!, d[pos + 6]!, d[pos + 7]!),
      start: pos,
      body: pos + 8,
      end: pos + size,
    };
    pos += size;
  }
}

function find(d: Uint8Array, start: number, end: number, path: string[]): Box {
  for (const b of boxes(d, start, end)) {
    if (b.type !== path[0]) continue;
    if (path.length === 1) return b;
    /* meta is a FullBox: its children start after version/flags. */
    return find(d, b.body + (b.type === "meta" ? 4 : 0), b.end, path.slice(1));
  }
  throw new Error(`box ${path[0]} not found`);
}

/** The coded primary image of a single-image HEIC. */
type Source = {
  ispe: Uint8Array; // whole boxes
  hvcC: Uint8Array;
  payload: Uint8Array;
  width: number;
  height: number;
};

function readSource(path: string): Source {
  const d = new Uint8Array(readFileSync(path));
  const ipco = find(d, 0, d.length, ["meta", "iprp", "ipco"]);
  const props = new Map<string, Uint8Array>();
  for (const b of boxes(d, ipco.body, ipco.end))
    if (!props.has(b.type)) props.set(b.type, d.subarray(b.start, b.end));
  const ispe = props.get("ispe");
  const hvcC = props.get("hvcC");
  if (!ispe || !hvcC) throw new Error(`${path}: need ispe + hvcC`);

  const primary = rb16(d, find(d, 0, d.length, ["meta", "pitm"]).body + 4);
  const iloc = find(d, 0, d.length, ["meta", "iloc"]);
  const version = d[iloc.body]!;
  const offSize = d[iloc.body + 4]! >> 4;
  const lenSize = d[iloc.body + 4]! & 15;
  const baseSize = d[iloc.body + 5]! >> 4;
  let p = iloc.body + 6;
  const count = rb16(d, p);
  p += 2;
  let payload: Uint8Array | null = null;
  for (let i = 0; i < count; i++) {
    const id = rb16(d, p);
    p += 2;
    if (version >= 1) p += 2; // construction_method
    p += 2; // data_reference_index
    const base = rbN(d, p, baseSize);
    p += baseSize;
    const extents = rb16(d, p);
    p += 2;
    const parts: Uint8Array[] = [];
    for (let e = 0; e < extents; e++) {
      const off = rbN(d, p, offSize);
      p += offSize;
      const len = rbN(d, p, lenSize);
      p += lenSize;
      parts.push(d.subarray(base + off, base + off + len));
    }
    if (id === primary) payload = concat(...parts);
  }
  if (!payload) throw new Error(`${path}: primary item data not found`);
  return { ispe, hvcC, payload, width: rb32(ispe, 12), height: rb32(ispe, 16) };
}

/* --- NAL helpers for the second PPS --- */

function unescapeRbsp(b: Uint8Array): number[] {
  const out: number[] = [];
  let zeros = 0;
  for (const x of b) {
    if (zeros >= 2 && x === 3) {
      zeros = 0;
      continue;
    }
    out.push(x);
    zeros = x === 0 ? zeros + 1 : 0;
  }
  return out;
}

function escapeRbsp(b: number[]): number[] {
  const out: number[] = [];
  let zeros = 0;
  for (const x of b) {
    if (zeros >= 2 && x <= 3) {
      out.push(3);
      zeros = 0;
    }
    out.push(x);
    zeros = x === 0 ? zeros + 1 : 0;
  }
  return out;
}

/** Copy of a PPS NAL with pps_pic_parameter_set_id changed from 0 to 1. */
function ppsWithId1(nal: Uint8Array): Uint8Array {
  const rbsp = unescapeRbsp(nal.subarray(2));
  let bits = rbsp.map((x) => x.toString(2).padStart(8, "0")).join("");
  if (bits[0] !== "1") throw new Error("source PPS id is not 0");
  /* drop ue(0) and the stop bit + padding; write ue(1), payload, stop bit */
  bits = "010" + bits.slice(1, bits.lastIndexOf("1")) + "1";
  bits += "0".repeat((8 - (bits.length % 8)) % 8);
  const out: number[] = [];
  for (let i = 0; i < bits.length; i += 8) out.push(parseInt(bits.slice(i, i + 8), 2));
  return new Uint8Array([nal[0]!, nal[1]!, ...escapeRbsp(out)]);
}

/** hvcC box with a second PPS (id 1) appended to its PPS array. */
function hvccWithSecondPps(hvcC: Uint8Array): Uint8Array {
  const h = hvcC.subarray(8); // box body
  const parts: Uint8Array[] = [h.subarray(0, 23)];
  let pos = 23;
  let done = false;
  for (let a = 0; a < h[22]!; a++) {
    const typeByte = h[pos]!;
    const n = rb16(h, pos + 1);
    pos += 3;
    const nals: Uint8Array[] = [];
    for (let i = 0; i < n; i++) {
      const len = rb16(h, pos);
      nals.push(h.subarray(pos + 2, pos + 2 + len));
      pos += 2 + len;
    }
    if ((typeByte & 0x3f) === 34) {
      nals.push(ppsWithId1(nals[0]!));
      done = true;
    }
    parts.push(new Uint8Array([typeByte]), be16(nals.length));
    for (const nal of nals) parts.push(be16(nal.length), nal);
  }
  if (!done) throw new Error("no PPS array in hvcC");
  return box("hvcC", concat(...parts));
}

/* --- container assembly --- */

type Item = {
  id: number;
  type: string;
  props: number[];
  idat?: Uint8Array;
  /** put this item's associations in a second ipma (16-bit indices) */
  secondIpma?: boolean;
};

function infe(id: number, type: string): Uint8Array {
  return fullbox("infe", 2, 0, concat(be16(id), be16(0), ascii(type), new Uint8Array([0])));
}

/**
 * ftyp + meta + mdat. Items without `idat` all point at the same coded image
 * in mdat; `idat` items (derived images) use construction_method 1.
 * Property indices are 1-based into `ipco`; every association is essential.
 */
function assemble(items: Item[], ipco: Uint8Array[], iref: Uint8Array, payload: Uint8Array): Uint8Array {
  const idatParts = items.filter((i) => i.idat).map((i) => i.idat!);
  const iloc = (mdatOffset: number): Uint8Array => {
    const entries: Uint8Array[] = [];
    let idatPos = 0;
    for (const it of items) {
      if (it.idat) {
        entries.push(be16(it.id), be16(1), be16(0), be16(1), be32(idatPos), be32(it.idat.length));
        idatPos += it.idat.length;
      } else {
        entries.push(be16(it.id), be16(0), be16(0), be16(1), be32(mdatOffset), be32(payload.length));
      }
    }
    return fullbox("iloc", 1, 0, concat(new Uint8Array([0x44, 0x00]), be16(items.length), ...entries));
  };
  const hdlr = fullbox("hdlr", 0, 0, concat(be32(0), ascii("pict"), new Uint8Array(13)));
  const pitm = fullbox("pitm", 0, 0, be16(items[0]!.id));
  const iinf = fullbox("iinf", 0, 0, concat(be16(items.length), ...items.map((i) => infe(i.id, i.type))));
  const first = items.filter((i) => !i.secondIpma);
  const second = items.filter((i) => i.secondIpma);
  const ipmaBoxes = [
    fullbox("ipma", 0, 0, concat(
      be32(first.length),
      ...first.map((i) =>
        concat(be16(i.id), new Uint8Array([i.props.length, ...i.props.map((p) => 0x80 | p)]))),
    )),
  ];
  if (second.length)
    ipmaBoxes.push(fullbox("ipma", 0, 1, concat(
      be32(second.length),
      ...second.map((i) =>
        concat(be16(i.id), new Uint8Array([i.props.length]), ...i.props.map((p) => be16(0x8000 | p)))),
    )));
  const iprp = box("iprp", concat(box("ipco", concat(...ipco)), ...ipmaBoxes));
  const meta = (mdatOffset: number) =>
    fullbox("meta", 0, 0, concat(
      hdlr, pitm, iloc(mdatOffset), iinf, iprp, iref,
      ...(idatParts.length ? [box("idat", concat(...idatParts))] : []),
    ));
  const ftyp = box("ftyp", concat(ascii("heic"), be32(0), ascii("mif1"), ascii("heic")));
  const offset = ftyp.length + meta(0).length + 8;
  return concat(ftyp, meta(offset), box("mdat", payload));
}

function write(path: string, data: Uint8Array): void {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, data);
  console.log(`wrote ${path} (${data.length} bytes)`);
}

export function generateRegressFixtures(
  sourcePath: string = DEFAULT_SOURCE,
  outDir: string = DEFAULT_OUT,
): void {
  const src = readSource(sourcePath);

  /* 2x1 grid, both tiles the same coded image, hvcC with PPS 0 and PPS 1. */
  {
    const grid = concat(new Uint8Array([0, 0, 0, 1]), be16(src.width * 2), be16(src.height));
    const gridIspe = fullbox("ispe", 0, 0, concat(be32(src.width * 2), be32(src.height)));
    write(join(outDir, "two_pps_grid.heic"), assemble(
      [
        { id: 1, type: "grid", props: [3], idat: grid },
        { id: 2, type: "hvc1", props: [1, 2] },
        { id: 3, type: "hvc1", props: [1, 2] },
      ],
      [src.ispe, hvccWithSecondPps(src.hvcC), gridIspe],
      fullbox("iref", 0, 0, box("dimg", concat(be16(1), be16(2), be16(2), be16(3)))),
      src.payload,
    ));
  }

  /* The same grid with plain hvcC, its associations split over two ipma
     boxes (flags 0 for the grid item, flags 1 for the tiles). The second box
     used to replace the first, so the grid lost its ispe. */
  {
    const grid = concat(new Uint8Array([0, 0, 0, 1]), be16(src.width * 2), be16(src.height));
    const gridIspe = fullbox("ispe", 0, 0, concat(be32(src.width * 2), be32(src.height)));
    write(join(outDir, "two_ipma_grid.heic"), assemble(
      [
        { id: 1, type: "grid", props: [3], idat: grid },
        { id: 2, type: "hvc1", props: [1, 2], secondIpma: true },
        { id: 3, type: "hvc1", props: [1, 2], secondIpma: true },
      ],
      [src.ispe, src.hvcC, gridIspe],
      fullbox("iref", 0, 0, box("dimg", concat(be16(1), be16(2), be16(2), be16(3)))),
      src.payload,
    ));
  }

  /* Overlay with a saturated background: the image at (16,16) on a canvas
     32 larger, filled with opaque red. The fill used to put R into luma
     with neutral chroma, so red came out as white. */
  {
    const cw = src.width + 32, ch = src.height + 32;
    const iovl = concat(
      new Uint8Array([0, 0]),
      be16(0xffff), be16(0), be16(0), be16(0xffff),
      be16(cw), be16(ch),
      be16(16), be16(16),
    );
    const canvasIspe = fullbox("ispe", 0, 0, concat(be32(cw), be32(ch)));
    write(join(outDir, "iovl_red_fill.heic"), assemble(
      [
        { id: 1, type: "iovl", props: [3], idat: iovl },
        { id: 2, type: "hvc1", props: [1, 2] },
      ],
      [src.ispe, src.hvcC, canvasIspe],
      fullbox("iref", 0, 0, box("dimg", concat(be16(1), be16(1), be16(2)))),
      src.payload,
    ));
  }

  /* The image with an unknown property ('zzzz') marked essential. A reader
     must not decode such an item (ISO 23008-12 6.5.1); we used to ignore
     the flag. Listed in EXPECT_FAIL. */
  write(join(outDir, "essential_unknown.heic"), assemble(
    [{ id: 1, type: "hvc1", props: [1, 2, 3] }],
    [src.ispe, src.hvcC, box("zzzz", new Uint8Array(4))],
    new Uint8Array(0),
    src.payload,
  ));

  /* The grid with no ispe on the grid item itself. heic_doc_info used to
     report the first tile's size instead of the grid's. */
  {
    const grid = concat(new Uint8Array([0, 0, 0, 1]), be16(src.width * 2), be16(src.height));
    write(join(outDir, "_info", "grid_no_ispe.heic"), assemble(
      [
        { id: 1, type: "grid", props: [], idat: grid },
        { id: 2, type: "hvc1", props: [1, 2] },
        { id: 3, type: "hvc1", props: [1, 2] },
      ],
      [src.ispe, src.hvcC],
      fullbox("iref", 0, 0, box("dimg", concat(be16(1), be16(2), be16(2), be16(3)))),
      src.payload,
    ));
  }

  /* Primary + an `auxl` item that has no auxC property. */
  write(join(outDir, "_info", "aux_no_auxc.heic"), assemble(
    [
      { id: 1, type: "hvc1", props: [1, 2] },
      { id: 2, type: "hvc1", props: [1, 2] },
    ],
    [src.ispe, src.hvcC],
    fullbox("iref", 0, 0, box("auxl", concat(be16(2), be16(1), be16(1)))),
    src.payload,
  ));
}

if (import.meta.main) {
  generateRegressFixtures(process.argv[2] ?? DEFAULT_SOURCE, process.argv[3] ?? DEFAULT_OUT);
}
