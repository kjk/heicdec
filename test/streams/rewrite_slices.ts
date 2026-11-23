// Rewrites an HM stream (two slices per picture) so that the second slice of
// each picture differs from the first in a way HM itself never produces.
//   lists: ref_pic_list_modification reverses L0 in the second slice
//   pps:   the second slice names a second PPS (same but init_qp + 2);
//          this is an INVALID stream (H.265 7.4.7.1), kept to show that
//          decoders reject it
// usage: bun rewrite_slices.ts base.bit offsets.txt lists|pps out.bit
// offsets.txt: the "PPS ..." / "SH ..." lines of the instrumented decoder
// (see README.md).
import { readFileSync, writeFileSync } from "node:fs";

const [src, offs, mode, dst] = process.argv.slice(2);
if (!src || !offs || (mode !== "lists" && mode !== "pps") || !dst) {
  console.error(
    "usage: bun rewrite_slices.ts base.bit offsets.txt lists|pps out.bit",
  );
  process.exit(2);
}

function assert(cond: unknown, msg: string): asserts cond {
  if (!cond) throw new Error(`rewrite_slices: ${msg}`);
}

/** NAL units of an Annex B stream, without start codes or trailing zeros. */
function splitAnnexB(d: Uint8Array): Uint8Array[] {
  const starts: number[] = [];
  for (let i = 0; i + 2 < d.length; i++) {
    if (d[i] === 0 && d[i + 1] === 0 && d[i + 2] === 1) {
      starts.push(i + 3);
      i += 2;
    }
  }
  return starts.map((s, k) => {
    let e = k + 1 === starts.length ? d.length : starts[k + 1]! - 3;
    while (e > s && d[e - 1] === 0) e--;
    return d.subarray(s, e);
  });
}

/** Remove emulation prevention bytes (00 00 03 -> 00 00). */
function unescapeRbsp(b: Uint8Array): Uint8Array {
  const out: number[] = [];
  let zeros = 0;
  for (const byte of b) {
    if (zeros >= 2 && byte === 3) {
      zeros = 0;
      continue;
    }
    out.push(byte);
    zeros = byte === 0 ? zeros + 1 : 0;
  }
  return Uint8Array.from(out);
}

/** Insert emulation prevention bytes. */
function escapeRbsp(b: Uint8Array): Uint8Array {
  const out: number[] = [];
  let zeros = 0;
  for (const byte of b) {
    if (zeros >= 2 && byte <= 3) {
      out.push(3);
      zeros = 0;
    }
    out.push(byte);
    zeros = byte === 0 ? zeros + 1 : 0;
  }
  return Uint8Array.from(out);
}

const bitsOf = (b: Uint8Array): string =>
  Array.from(b, (x) => x.toString(2).padStart(8, "0")).join("");

function bytesOf(s: string): Uint8Array {
  assert(s.length % 8 === 0, "bit string is not byte aligned");
  const out = new Uint8Array(s.length / 8);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(s.slice(i * 8, i * 8 + 8), 2);
  }
  return out;
}

/** Exp-Golomb ue(v) / se(v) as bit strings. */
function ue(v: number): string {
  const code = (v + 1).toString(2);
  return "0".repeat(code.length - 1) + code;
}
const se = (v: number): string => ue(v > 0 ? 2 * v - 1 : -2 * v);

/** Append the stop bit and pad to a byte boundary. */
function align(s: string): string {
  const padded = s + "1";
  return padded + "0".repeat((8 - (padded.length % 8)) % 8);
}

/** Bits needed for values 0..n-1: Ceil(Log2(n)). */
const ceilLog2 = (n: number): number => (n <= 1 ? 0 : (n - 1).toString(2).length);

function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

type Fields = Record<string, number>;
function fields(line: string): Fields {
  const out: Fields = {};
  for (const m of line.matchAll(/(\w+)=(-?\d+)/g)) out[m[1]!] = Number(m[2]);
  return out;
}

const lines = readFileSync(offs, "utf8").split(/\r?\n/);
const ppsInfo = lines.filter((l) => l.startsWith("PPS")).map(fields)[0];
const shInfo = lines.filter((l) => l.startsWith("SH")).map(fields);
assert(ppsInfo, "no PPS line in the offsets file");

const out: Uint8Array[] = [];
let si = 0;
for (const nal of splitAnnexB(new Uint8Array(readFileSync(src)))) {
  const type = (nal[0]! >> 1) & 63;
  if (type === 39 || type === 40) continue; // SEI: picture hashes no longer apply
  const hdr = nal.subarray(0, 2);
  const rbsp = unescapeRbsp(nal.subarray(2));
  const b = bitsOf(rbsp);

  if (type === 34) { // PPS
    if (mode === "lists") {
      const p = ppsInfo.lm_at!;
      assert(b[p] === "0", "lists_modification_present_flag already set");
      out.push(concat(hdr, escapeRbsp(bytesOf(b.slice(0, p) + "1" + b.slice(p + 1)))));
    } else {
      out.push(nal);
      const a = ppsInfo.initqp_at!;
      const e = ppsInfo.initqp_end!;
      assert(b[0] === "1", "PPS id is not 0");
      const body = b.slice(0, b.lastIndexOf("1")); // drop rbsp_trailing_bits
      const nb = ue(1) + body.slice(1, a) + se(ppsInfo.initqp! + 2) + body.slice(e);
      out.push(concat(hdr, escapeRbsp(bytesOf(align(nb)))));
    }
    continue;
  }
  if (type > 31) {
    out.push(nal);
    continue;
  }

  const info = shInfo[si++];
  assert(info, "more slice NAL units than SH lines");
  const end = info.end!;
  const dataStart = Math.floor(end / 8) + 1;
  let head = b.slice(0, end);
  const tail = rbsp.subarray(dataStart);
  assert(
    b[end] === "1" && !b.slice(end + 1, dataStart * 8).includes("1"),
    "slice header end does not sit on byte_alignment()",
  );
  const second = info.first === 0;
  if (mode === "lists") {
    // slice_type: 0 = B, 1 = P, 2 = I
    if (info.type !== 2 && info.used! > 1) {
      const p = info.rplm_at!;
      let ins = "0";
      if (second && info.n0! > 1) {
        const n = info.n0!;
        const width = ceilLog2(info.used!);
        assert(
          n <= info.used! && !info.col_l0,
          "L0 reversal would move the collocated picture",
        );
        ins = "1";
        for (let i = 0; i < n; i++) {
          ins += (n - 1 - i).toString(2).padStart(width, "0");
        }
      }
      if (info.type === 0) ins += "0"; // ref_pic_list_modification_flag_l1
      head = head.slice(0, p) + ins + head.slice(p);
    }
  } else if (second) {
    const p = info.pps_at!;
    assert(head[p] === "1", "slice PPS id is not 0");
    head = head.slice(0, p) + ue(1) + head.slice(p + 1);
  }
  out.push(concat(hdr, escapeRbsp(concat(bytesOf(align(head)), tail))));
}
assert(si === shInfo.length, `used ${si} of ${shInfo.length} SH lines`);

const startCode = Uint8Array.of(0, 0, 0, 1);
writeFileSync(dst, concat(...out.flatMap((nal) => [startCode, nal])));
console.log(`${dst}: ${out.length} NAL units`);
