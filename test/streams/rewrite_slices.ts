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
import {
  align,
  assert,
  bitsOf,
  buildSlice,
  bytesOf,
  ceilLog2,
  concat,
  escapeRbsp,
  joinAnnexB,
  nalType,
  offsetLines,
  se,
  splitAnnexB,
  splitSlice,
  ue,
  unescapeRbsp,
} from "./annexb.ts";

const [src, offs, mode, dst] = process.argv.slice(2);
if (!src || !offs || (mode !== "lists" && mode !== "pps") || !dst) {
  console.error(
    "usage: bun rewrite_slices.ts base.bit offsets.txt lists|pps out.bit",
  );
  process.exit(2);
}

const offsets = readFileSync(offs, "utf8");
const ppsInfo = offsetLines(offsets, "PPS")[0];
const shInfo = offsetLines(offsets, "SH");
assert(ppsInfo, "no PPS line in the offsets file");

const out: Uint8Array[] = [];
let si = 0;
for (const nal of splitAnnexB(new Uint8Array(readFileSync(src)))) {
  const type = nalType(nal);
  if (type === 39 || type === 40) continue; // SEI: picture hashes no longer apply

  if (type === 34) { // PPS
    const hdr = nal.subarray(0, 2);
    const b = bitsOf(unescapeRbsp(nal.subarray(2)));
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
  const slice = splitSlice(nal, info.end!);
  let head = slice.head;
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
  out.push(buildSlice(slice.nalHeader, head, slice.data));
}
assert(si === shInfo.length, `used ${si} of ${shInfo.length} SH lines`);

writeFileSync(dst, joinAnnexB(out));
console.log(`${dst}: ${out.length} NAL units`);
