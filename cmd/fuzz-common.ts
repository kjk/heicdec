// fuzz-common.ts -- what cmd/fuzz.ts (libFuzzer) and cmd/fuzz-afl.ts (AFL++)
// share: where each fuzz target keeps its corpus and crashes, its dictionary
// and input size cap, and how its corpus is seeded.
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { basename, join, resolve } from "node:path";
import { type FuzzTarget } from "./build";
import { corpusFiles } from "./corpus";
import { TESTIMAGES_DIR } from "./get-deps";

export const ROOT = resolve(import.meta.dir, "..");
export const FUZZ = join(ROOT, "fuzz");

export type FuzzTargetDirs = {
  /** fuzz/<corpus>: gitignored, the fuzzer's checkpoint */
  corpus: string;
  /** fuzz/<crashes>: tracked, artifacts become regression inputs */
  crashes: string;
  /** fuzz/<dict>: tracked libFuzzer / AFL++ dictionary */
  dict: string;
  /** default input size cap in bytes */
  maxLen: number;
};

/* Small inputs are the point: the fuzzers' speed and their mutation
   precision both fall with input size. Whole files get 256 KB (a small
   photo still fits); the parser and raw-stream targets 64 KB. */
export const FUZZ_DIRS: Record<FuzzTarget, FuzzTargetDirs> = {
  heic: { corpus: "corpus", crashes: "crashes", dict: "heic.dict", maxLen: 256 * 1024 },
  container: {
    corpus: "corpus-container",
    crashes: "crashes-container",
    dict: "heic.dict",
    maxLen: 64 * 1024,
  },
  hevc: {
    corpus: "corpus-hevc",
    crashes: "crashes-hevc",
    dict: "hevc.dict",
    maxLen: 64 * 1024,
  },
  diff: {
    corpus: "corpus-diff",
    crashes: "crashes-diff",
    dict: "heic.dict",
    maxLen: 256 * 1024,
  },
};

export const corpusDir = (t: FuzzTarget) => join(FUZZ, FUZZ_DIRS[t].corpus);
export const crashesDir = (t: FuzzTarget) => join(FUZZ, FUZZ_DIRS[t].crashes);
export const dictPath = (t: FuzzTarget) => join(FUZZ, FUZZ_DIRS[t].dict);

// Artifact kinds libFuzzer writes on a finding (all reproducible by replay),
// plus crashes imported by fuzz-afl.ts (afl-<sha1>).
export const isArtifact = (name: string) =>
  /^(crash|timeout|oom|leak|slow-unit|afl)-/.test(name);

export function listArtifacts(t: FuzzTarget): string[] {
  const dir = crashesDir(t);
  if (!existsSync(dir)) return [];
  return readdirSync(dir).filter(isArtifact);
}

/* ---------------- seeding ---------------- */

function walk(dir: string, exts: string[]): string[] {
  if (!existsSync(dir)) return [];
  const out: string[] = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, e.name);
    if (e.isDirectory()) out.push(...walk(p, exts));
    else if (exts.some((x) => e.name.toLowerCase().endsWith(x))) out.push(p);
  }
  return out.sort();
}

/** Write `data` (cut to `maxLen`) as a seed; never overwrites. Returns 1 if written. */
function writeSeed(dir: string, name: string, data: Uint8Array, maxLen: number): number {
  const cut = data.length > maxLen;
  let dest = join(dir, cut ? `trunc_${name}` : name);
  for (let n = 1; existsSync(dest); n++) {
    if (statSync(dest).size === Math.min(data.length, maxLen)) return 0;
    dest = join(dir, `${n}_${cut ? "trunc_" : ""}${name}`);
  }
  writeFileSync(dest, cut ? data.subarray(0, maxLen) : data);
  return 1;
}

const rb32 = (d: Uint8Array, o: number) =>
  ((d[o]! << 24) | (d[o + 1]! << 16) | (d[o + 2]! << 8) | d[o + 3]!) >>> 0;

/**
 * The HEVC stream of a simple HEIC file as Annex B: the parameter sets from
 * its first hvcC, then the NAL units of mdat (length-prefixed there). Returns
 * null when the file has no hvcC or mdat does not start with NAL units
 * (grids and files with several items still yield their first picture).
 */
export function heicToAnnexB(d: Uint8Array): Uint8Array | null {
  const find = (fourcc: string, from = 0): number => {
    const c = [...fourcc].map((ch) => ch.charCodeAt(0));
    for (let i = from + 4; i + 4 <= d.length; i++)
      if (d[i] === c[0] && d[i + 1] === c[1] && d[i + 2] === c[2] && d[i + 3] === c[3])
        return i - 4;
    return -1;
  };
  const start = [0, 0, 0, 1];
  const out: number[] = [];
  const hvcC = find("hvcC");
  if (hvcC < 0) return null;
  const hvcEnd = Math.min(d.length, hvcC + rb32(d, hvcC));
  let p = hvcC + 8 + 21; // box header + fields before lengthSizeMinusOne
  if (p + 2 > hvcEnd) return null;
  const lengthSize = (d[p]! & 3) + 1;
  const arrays = d[p + 1]!;
  p += 2;
  for (let a = 0; a < arrays && p + 3 <= hvcEnd; a++) {
    const count = (d[p + 1]! << 8) | d[p + 2]!;
    p += 3;
    for (let n = 0; n < count && p + 2 <= hvcEnd; n++) {
      const len = (d[p]! << 8) | d[p + 1]!;
      p += 2;
      if (p + len > hvcEnd) return null;
      out.push(...start, ...d.subarray(p, p + len));
      p += len;
    }
  }
  const mdat = find("mdat");
  if (mdat < 0) return null;
  const size = rb32(d, mdat);
  const end = size === 0 ? d.length : Math.min(d.length, mdat + size);
  let q = mdat + 8;
  let nals = 0;
  while (q + lengthSize <= end) {
    let len = 0;
    for (let i = 0; i < lengthSize; i++) len = len * 256 + d[q + i]!;
    q += lengthSize;
    if (len < 2 || q + len > end) break;
    /* forbidden_zero_bit must be 0 and the layer id small: otherwise this
       is not NAL data (EXIF, a second item, ...). */
    if ((d[q]! & 0x80) !== 0) break;
    out.push(...start, ...d.subarray(q, q + len));
    q += len;
    nals++;
  }
  return nals ? Uint8Array.from(out) : null;
}

/** Raw HEVC streams: conformance sets and the checked-in ones. */
function rawHevcStreams(): string[] {
  return [
    ...walk(join(ROOT, "test", "streams"), [".bit"]),
    ...walk(join(TESTIMAGES_DIR, "hevc_sequence"), [".bit", ".bin"]),
    ...walk(join(TESTIMAGES_DIR, "hevc_rext"), [".bit", ".bin"]),
  ];
}

/**
 * Add seeds for `target` to its corpus directory (existing files are kept).
 * Inputs larger than `maxLen` are not dropped but cut to it: the head of a
 * big file still carries its whole box structure / parameter sets, which is
 * what the mutators need. Returns the number of files written.
 */
export function seedCorpus(target: FuzzTarget, maxLen: number): number {
  const dir = corpusDir(target);
  mkdirSync(dir, { recursive: true });
  let n = 0;
  /* Tracked seeds made for the blind spots a coverage report showed
     (fuzz/seeds-<target>/, e.g. fuzz/gen_hevc_seeds.ts). */
  const tracked = join(FUZZ, `seeds-${target}`);
  if (existsSync(tracked))
    for (const name of readdirSync(tracked))
      n += writeSeed(dir, name, new Uint8Array(readFileSync(join(tracked, name))), maxLen);
  if (target === "hevc") {
    for (const f of rawHevcStreams())
      n += writeSeed(dir, basename(f), new Uint8Array(readFileSync(f)), maxLen);
    for (const f of corpusFiles()) {
      if (!/\.(heic|heif)$/i.test(f)) continue;
      const annexb = heicToAnnexB(new Uint8Array(readFileSync(f)));
      if (annexb) n += writeSeed(dir, `${basename(f)}.bit`, annexb, maxLen);
    }
    return n;
  }
  for (const f of corpusFiles()) {
    const size = statSync(f).size;
    /* A cut file cannot decode, so it is no use to the differential target. */
    if (target === "diff" && size > maxLen) continue;
    n += writeSeed(dir, basename(f), new Uint8Array(readFileSync(f)), maxLen);
  }
  return n;
}

/** Copy every file of `from` that `to` does not have yet (flat). */
export function copyNew(from: string, to: string): number {
  if (!existsSync(from)) return 0;
  mkdirSync(to, { recursive: true });
  let n = 0;
  for (const name of readdirSync(from)) {
    const dest = join(to, name);
    if (existsSync(dest)) continue;
    copyFileSync(join(from, name), dest);
    n++;
  }
  return n;
}
