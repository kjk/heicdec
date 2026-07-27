// Wrap the first access unit of an Annex B HEVC conformance stream in HEIF.
import { mkdirSync, readFileSync, writeFileSync } from "fs";
import { dirname } from "path";

const HIGH_PRECISION_WEIGHTING_HEVC =
  "AAAAAUABDAH//wQIAAADAAmIAAADAAADAJcCQAAAAAFCAQEECAAAAwAJiAAAAwAA" +
  "AwCgIIEH7ZZeSRtB5eSST+efzy////z+fz8/nbYACQAAAAFEAcGQkaESAAABJgGs" +
  "TkBLB6m3GWVYeLKkf/c65mH34NWr71XSFk7QFw+zQJYSJcLSTw5lkj7NrhpPUrj" +
  "j2NBWjnaiV3JVuEfHzbuh0F6fXrkSVTMIAkJVk5/wSEMPwLVVd725zjgxl76/IO" +
  "t7nuNHcORGkbotPzZgmY+LzKEJTQJAzUsusaW5gQCrNq31+sI2xn6M03GcADQqTh" +
  "5/C2loLAHgQ2UQSMP5Bd0gtuxwCkgPdEvC5vuza4TOv89houbHFUxof32M85aWk" +
  "e90sAyeyCrSNofiyx2JbBFZjaIyma+i1xGiJc0wy+v9iB7M2eI2QjV8BfRJTzno" +
  "2qmlfRdzaOSAXCKmQ3wDmOqnQ31dK0m+U9z4MvNPAaBjWOLH8UysYFlRxVtsIrx" +
  "s+e0N3BC37OrDJTCwzRb4Ixoms68SMnvV8cvucWCrb+mNiakf0Q+EL5nA4SByLf" +
  "ha9b+2RMcg6gIkjvjdCSE9CU35swZORy9S0jO78ixzoZmNBPx3Yu+MkiyhrybPe" +
  "2Ff5PGVG7wY1jNAQkV81hePQvEgAAAAAQIB0A04/gaoPoF68L3gAAAAAQIB0BV1n" +
  "/A1QfQL0BqQPkBeS4C64AAAAAECAdAdt5/8DVB9AvQGpA+QF5AJ9AXUAjV4veA=";

const SAO_OFFSET_SCALE_HEVC =
  "AAAAAUABDAH//wQIAAADAAmIAAADAAADAPAkAAAAAUIBAQQIAAADAAmIAAADAAAD" +
  "AKAQIGH5Sl+SRtntkAAAAAFEAcGQkYEWAE4AAAEmAa8TgNuRbVizfNS+tRNAqAeg" +
  "ZJpKG/zkEovamZpDQ3XdV52hxzkJVa0tiO9JpwYyfTT1T55YA4tJv1vaewNAy0w/" +
  "8OF9zV0/RJIpf/A+C+s2hgbbiDJX4I5Ax488mbWygTlLAWY24jfA8Cuwb03uLcYz" +
  "VaX3T0ahA+bbBMPsQV4ButzP2AgH8djQ2EWLkzV/dyMjxtDZVFb794+o2gMfOXSB" +
  "+B+X7mogYwSejeqeJcBuLv3c4DdAoxqMFE3FiJYMnDnCUcHep4Q8DDWeshfKXVvm" +
  "tq95e2K/5p/TYzm7FzGUt+ANEKR3AhJ/p1/jnxQW+hT8hvaW+cpEWtq0lRJiYPU2" +
  "80faCKMnrssgivyWxBKZtCLirt0dWMCG4fe1Nyyn3mIMHeSko4ELUXr/cmswsn7D" +
  "m/a45HZxznOskondylOXbk/2G6XfFqcyJjs4vdDACVlk8OcLHd/wL1Txbw2ReXTP" +
  "Xr8sTT2etQozrvrXrKFftKN5W9oZAVjjYFWfN7nu4QlGmcXf4tDF8Dc2h0o62/7a" +
  "DUtHpzGEmqyWBKiQfupVnjblZzyh3wjgoCTNvfYetLLqMvZnps/M6pVHh6wpLS2j" +
  "QZeEtLFaB7mMgD5NpTjO35BPqsHVk3uJd5Nyc0jCmv80LHYc4B5vCa6CdXgkBT3p" +
  "CKPcA4l0/0pjhOR5FzdsOHw2lOxaPm+Uhg0qFnWO7ipTEOD8pSlcnFbvXjViM4gB" +
  "Njxq6v6gBJ6mTF+w5837fYVYolyUWH4+J1uD8ODDkS4tjgxuxsK18Nc0GwdnDlkS" +
  "GVSayft1TZNtHGXCWpxGL4+UNP0XWmyRA6uQCVRN+QHAgHSMB1XnrIc+tc98mMa6" +
  "+xBxuCg8FZja9iNTxKXq7UAAjkkY1oYoFtXNodm8XGwPI1MRC0FcWw/n9MVGKRKJ" +
  "dZT+ml90yZnhCBZ0TvuGp/n9hojyTJlu5fb55xKw";

function writeEmbeddedHevc(outputPath: string, encoded: string): void {
  const data = Uint8Array.from(atob(encoded), (c) => c.charCodeAt(0));
  mkdirSync(dirname(outputPath), { recursive: true });
  writeFileSync(outputPath, data);
  console.log(`wrote ${outputPath} (${data.length} bytes)`);
}

/** Install a 64x64 10-bit HM stream with nonzero high-precision P offsets. */
export function generateHighPrecisionWeightingHevc(outputPath: string): void {
  writeEmbeddedHevc(outputPath, HIGH_PRECISION_WEIGHTING_HEVC);
}

/** Install a 128x96 12-bit HM stream with luma/chroma SAO shifts 1/2. */
export function generateSaoOffsetScaleHevc(outputPath: string): void {
  writeEmbeddedHevc(outputPath, SAO_OFFSET_SCALE_HEVC);
}

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

function hvccBox(
  params: Nal[],
  bitDepthLuma: number,
  bitDepthChroma: number,
  chromaFormat: number,
  profileIdc: number,
): Uint8Array {
  const header = new Uint8Array(23);
  header[0] = 1;
  header[1] = profileIdc;
  header[12] = 120;
  header[13] = 0xf0;
  header[14] = 0;
  header[15] = 0xfc;
  header[16] = 0xfc | chromaFormat;
  header[17] = 0xf8 | (bitDepthLuma - 8);
  header[18] = 0xf8 | (bitDepthChroma - 8);
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
  bitDepthLuma = 8,
  bitDepthChroma = bitDepthLuma,
  chromaFormat = 1,
  profileIdc = bitDepthLuma > 8 ? 2 : 1,
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
  const ipco = box(
    "ipco",
    concat(ispe, hvccBox(
      params,
      bitDepthLuma,
      bitDepthChroma,
      chromaFormat,
      profileIdc,
    )),
  );
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
