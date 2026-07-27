// Wrap the first access unit of an Annex B HEVC conformance stream in HEIF.
import { mkdirSync, readFileSync, writeFileSync } from "fs";
import { dirname } from "path";

function be16(n: number): Uint8Array {
  return new Uint8Array([(n >>> 8) & 0xff, n & 0xff]);
}

function be32(n: number): Uint8Array {
  return new Uint8Array([
    (n >>> 24) & 0xff,
    (n >>> 16) & 0xff,
    (n >>> 8) & 0xff,
    n & 0xff,
  ]);
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

function box(type: string, payload: Uint8Array): Uint8Array {
  return concat(
    be32(payload.length + 8),
    new TextEncoder().encode(type),
    payload,
  );
}

function fullbox(
  type: string,
  version: number,
  flags: number,
  payload: Uint8Array,
): Uint8Array {
  return box(type, concat(
    new Uint8Array([
      version,
      (flags >>> 16) & 0xff,
      (flags >>> 8) & 0xff,
      flags & 0xff,
    ]),
    payload,
  ));
}

type Nal = { data: Uint8Array; type: number };

function splitAnnexB(data: Uint8Array): Nal[] {
  const starts: { pos: number; prefix: number }[] = [];
  for (let i = 0; i + 3 <= data.length;) {
    let prefix = 0;
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1)
      prefix = 3;
    else if (
      i + 4 <= data.length && data[i] === 0 && data[i + 1] === 0
      && data[i + 2] === 0 && data[i + 3] === 1
    )
      prefix = 4;
    if (prefix) {
      starts.push({ pos: i, prefix });
      i += prefix;
    } else {
      i++;
    }
  }
  const nals: Nal[] = [];
  for (let i = 0; i < starts.length; i++) {
    const start = starts[i]!.pos + starts[i]!.prefix;
    let end = i + 1 < starts.length ? starts[i + 1]!.pos : data.length;
    while (end > start && data[end - 1] === 0) end--;
    if (end >= start + 2) {
      const nal = data.slice(start, end);
      nals.push({ data: nal, type: (nal[0]! >>> 1) & 0x3f });
    }
  }
  return nals;
}

function hvccBox(params: Nal[]): Uint8Array {
  const header = new Uint8Array(23);
  header[0] = 1;
  header[1] = 1;
  header[12] = 120;
  header[13] = 0xf0;
  header[14] = 0;
  header[15] = 0xfc;
  header[16] = 0xfd;
  header[17] = 0xf8;
  header[18] = 0xf8;
  header[21] = 3;
  header[22] = params.length;
  const arrays = params.map((nal) =>
    concat(
      new Uint8Array([0x80 | nal.type]),
      be16(1),
      be16(nal.data.length),
      nal.data,
    )
  );
  return box("hvcC", concat(header, ...arrays));
}

function infe(): Uint8Array {
  return fullbox("infe", 2, 0, concat(
    be16(1),
    be16(0),
    new TextEncoder().encode("hvc1"),
    new Uint8Array([0]),
  ));
}

function iloc(offset: number, length: number): Uint8Array {
  return fullbox("iloc", 1, 0, concat(
    new Uint8Array([0x44, 0]),
    be16(1),
    be16(1),
    be16(0),
    be16(0),
    be16(1),
    be32(offset),
    be32(length),
  ));
}

export function generateHevcHeif(
  sourcePath: string,
  outputPath: string,
  width: number,
  height: number,
): void {
  const nals = splitAnnexB(new Uint8Array(readFileSync(sourcePath)));
  const params = [32, 33, 34].map((type) => {
    const nal = nals.find((candidate) => candidate.type === type);
    if (!nal) throw new Error(`missing HEVC parameter set ${type}`);
    return nal;
  });
  const firstVcl = nals.findIndex((nal) =>
    nal.type <= 31 && nal.data.length > 2 && (nal.data[2]! & 0x80) !== 0
  );
  if (firstVcl < 0) throw new Error("missing first HEVC access unit");
  let endVcl = firstVcl + 1;
  while (
    endVcl < nals.length
    && !(nals[endVcl]!.type <= 31 && nals[endVcl]!.data.length > 2
      && (nals[endVcl]!.data[2]! & 0x80) !== 0)
  )
    endVcl++;
  const sampleParts: Uint8Array[] = [];
  for (const nal of nals.slice(firstVcl, endVcl)) {
    if (nal.type <= 31) sampleParts.push(be32(nal.data.length), nal.data);
  }
  const sample = concat(...sampleParts);
  const ispe = fullbox("ispe", 0, 0, concat(be32(width), be32(height)));
  const ipco = box("ipco", concat(ispe, hvccBox(params)));
  const ipma = fullbox("ipma", 0, 0, concat(
    be32(1),
    be16(1),
    new Uint8Array([2, 0x81, 0x82]),
  ));
  const iprp = box("iprp", concat(ipco, ipma));
  const iinf = fullbox("iinf", 0, 0, concat(be16(1), infe()));
  const pitm = fullbox("pitm", 0, 0, be16(1));
  const hdlr = fullbox("hdlr", 0, 0, concat(
    be32(0),
    new TextEncoder().encode("pict"),
    be32(0),
    be32(0),
    be32(0),
    new Uint8Array([0]),
  ));
  const ftyp = box("ftyp", concat(
    new TextEncoder().encode("heic"),
    be32(0),
    new TextEncoder().encode("mif1"),
    new TextEncoder().encode("heic"),
  ));
  const metaBase = concat(hdlr, pitm, iinf, iprp);
  let meta = fullbox("meta", 0, 0, concat(metaBase, iloc(0, sample.length)));
  for (let i = 0; i < 2; i++) {
    const offset = ftyp.length + meta.length + 8;
    meta = fullbox(
      "meta",
      0,
      0,
      concat(metaBase, iloc(offset, sample.length)),
    );
  }
  const output = concat(ftyp, meta, box("mdat", sample));
  mkdirSync(dirname(outputPath), { recursive: true });
  writeFileSync(outputPath, output);
  console.log(`wrote ${outputPath} (${output.length} bytes)`);
}
