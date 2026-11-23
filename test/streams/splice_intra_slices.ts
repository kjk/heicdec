// Makes pictures that mix intra and inter slices, which HM never encodes.
//
// Inputs are two HM encodes of the same video with the same slice layout
// (two slices per picture) and the same PPS: one with B slices, one
// all-intra. In the inter stream, one slice of each picture from the third
// on is replaced by the intra-coded slice of the same picture and position:
// alternately the second slice and the first. The slice header stays the
// inter stream's (same POC, reference picture set, NAL type), turned into an
// I-slice header: slice_type becomes I, the P/B-only fields are dropped, and
// the SAO flags and slice_qp_delta are the intra encode's, since the slice
// data was coded with them.
//
// usage: bun splice_intra_slices.ts inter.bit inter_offsets.txt \
//                                   intra.bit intra_offsets.txt out.bit
// The offsets files hold one "SH ..." line per slice NAL from the
// instrumented decoder (see README.md).
import { readFileSync, writeFileSync } from "node:fs";
import {
  assert,
  bitsOf,
  buildSlice,
  joinAnnexB,
  nalType,
  offsetLines,
  splitAnnexB,
  splitSlice,
  ue,
  unescapeRbsp,
} from "./annexb.ts";

const [interPath, interOffs, intraPath, intraOffs, dst] = process.argv.slice(2);
if (!interPath || !interOffs || !intraPath || !intraOffs || !dst) {
  console.error(
    "usage: bun splice_intra_slices.ts inter.bit inter_offsets.txt "
      + "intra.bit intra_offsets.txt out.bit",
  );
  process.exit(2);
}

const SLICE_I = 2;
const isSlice = (nal: Uint8Array) => nalType(nal) <= 31;

const interNals = splitAnnexB(new Uint8Array(readFileSync(interPath)));
const intraSlices = splitAnnexB(new Uint8Array(readFileSync(intraPath)))
  .filter(isSlice);
const interInfo = offsetLines(readFileSync(interOffs, "utf8"), "SH");
const intraInfo = offsetLines(readFileSync(intraOffs, "utf8"), "SH");
assert(
  interNals.filter(isSlice).length === interInfo.length
    && intraSlices.length === intraInfo.length
    && interInfo.length === intraInfo.length,
  "slice counts of the streams and offsets files differ",
);

const out: Uint8Array[] = [];
let si = 0;
let picture = -1;
let replaced = 0;
for (const nal of interNals) {
  const type = nalType(nal);
  if (type === 39 || type === 40) continue; // SEI: picture hashes no longer apply
  if (!isSlice(nal)) {
    out.push(nal);
    continue;
  }
  const a = interInfo[si]!;
  const b = intraInfo[si]!;
  const intraNal = intraSlices[si]!;
  si++;
  if (a.first === 1) picture++;
  assert(a.first === b.first, "slice layouts differ");

  // Even pictures get an intra second slice, odd ones an intra first slice.
  const replaceSecond = picture % 2 === 0;
  const target = picture >= 2 && a.type !== SLICE_I
    && (replaceSecond ? a.first === 0 : a.first === 1);
  if (!target) {
    out.push(nal);
    continue;
  }

  const inter = splitSlice(nal, a.end!);
  const intra = splitSlice(intraNal, b.end!);
  const intraBits = bitsOf(unescapeRbsp(intraNal.subarray(2)));
  const typeBits = ue(a.type!).length;
  const head = inter.head.slice(0, a.st_at!)
    + ue(SLICE_I)
    + inter.head.slice(a.st_at! + typeBits, a.sao_at!)
    + intraBits.slice(b.sao_at!, b.pb_at!) // slice_sao_luma/chroma_flag
    // the P/B-only fields [pb_at, qp_at) of the inter header are dropped
    + intraBits.slice(b.qp_at!, b.qp_end!) // slice_qp_delta
    + inter.head.slice(a.qp_end!);
  out.push(buildSlice(inter.nalHeader, head, intra.data));
  replaced++;
}

writeFileSync(dst, joinAnnexB(out));
console.log(`${dst}: ${out.length} NAL units, ${replaced} slices replaced`);
