// Shared helpers for the scripts that rewrite HEVC Annex B streams.

export function assert(cond: unknown, msg: string): asserts cond {
  if (!cond) throw new Error(msg);
}

/** NAL units of an Annex B stream, without start codes or trailing zeros. */
export function splitAnnexB(d: Uint8Array): Uint8Array[] {
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

/** nal_unit_type of a NAL unit (its first byte is the NAL header). */
export const nalType = (nal: Uint8Array): number => (nal[0]! >> 1) & 63;

/** Remove emulation prevention bytes (00 00 03 -> 00 00). */
export function unescapeRbsp(b: Uint8Array): Uint8Array {
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
export function escapeRbsp(b: Uint8Array): Uint8Array {
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

export const bitsOf = (b: Uint8Array): string =>
  Array.from(b, (x) => x.toString(2).padStart(8, "0")).join("");

export function bytesOf(s: string): Uint8Array {
  assert(s.length % 8 === 0, "bit string is not byte aligned");
  const out = new Uint8Array(s.length / 8);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(s.slice(i * 8, i * 8 + 8), 2);
  }
  return out;
}

/** Exp-Golomb ue(v) / se(v) as bit strings. */
export function ue(v: number): string {
  const code = (v + 1).toString(2);
  return "0".repeat(code.length - 1) + code;
}
export const se = (v: number): string => ue(v > 0 ? 2 * v - 1 : -2 * v);

/** Append the stop bit and pad to a byte boundary. */
export function align(s: string): string {
  const padded = s + "1";
  return padded + "0".repeat((8 - (padded.length % 8)) % 8);
}

/** Bits needed for values 0..n-1: Ceil(Log2(n)). */
export const ceilLog2 = (n: number): number =>
  n <= 1 ? 0 : (n - 1).toString(2).length;

export function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) {
    out.set(p, at);
    at += p.length;
  }
  return out;
}

/** The NAL units as an Annex B stream with 4-byte start codes. */
export function joinAnnexB(nals: Uint8Array[]): Uint8Array {
  const startCode = Uint8Array.of(0, 0, 0, 1);
  return concat(...nals.flatMap((nal) => [startCode, nal]));
}

export type Fields = Record<string, number>;

/** "name=123" pairs of one line of an offsets file. */
export function fields(line: string): Fields {
  const out: Fields = {};
  for (const m of line.matchAll(/(\w+)=(-?\d+)/g)) out[m[1]!] = Number(m[2]);
  return out;
}

/** The lines of an offsets file that start with `prefix`, parsed. */
export function offsetLines(text: string, prefix: string): Fields[] {
  return text.split(/\r?\n/).filter((l) => l.startsWith(prefix)).map(fields);
}

/**
 * A slice NAL split at the end of its header: the header bits (without
 * byte_alignment) and the slice data bytes. `end` is the header length in
 * RBSP bits, as reported by the instrumented decoder.
 */
export function splitSlice(nal: Uint8Array, end: number) {
  const rbsp = unescapeRbsp(nal.subarray(2));
  const bits = bitsOf(rbsp);
  const dataStart = Math.floor(end / 8) + 1;
  assert(
    bits[end] === "1" && !bits.slice(end + 1, dataStart * 8).includes("1"),
    "slice header end does not sit on byte_alignment()",
  );
  return {
    nalHeader: nal.subarray(0, 2),
    head: bits.slice(0, end),
    data: rbsp.subarray(dataStart),
  };
}

/** Rebuild a slice NAL from header bits and slice data. */
export function buildSlice(
  nalHeader: Uint8Array,
  head: string,
  data: Uint8Array,
): Uint8Array {
  return concat(nalHeader, escapeRbsp(concat(bytesOf(align(head)), data)));
}
