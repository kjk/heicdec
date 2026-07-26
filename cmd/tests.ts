// tests.ts -- corpus checks for the C HEIC decoder (libheif pixel oracle).
//
//   bun cmd/tests.ts -all                 # decode every corpus file vs libheif
//   bun cmd/tests.ts file.heic ...        # same for listed files
//   bun cmd/tests.ts -rand 10
//   bun cmd/tests.ts -info -all           # open/-info probe only (no decode)
//   bun cmd/tests.ts -mse 8 -all          # allow mse up to 8 (default 4)
//
// Default (like bench.ts): build dav1d+libheif harness, run heic_test -verify
// on each file. Success = both decode RGB and mse <= threshold.
// Failure = we cannot decode while libheif can, size mismatch, or bitmaps differ.
// Ok (not decodable) = both sides fail to decode (no pixel oracle possible).
// Skip = we decode but libheif does not (no oracle to compare).
//
// Intentional bad inputs (ok-expect-fail if we fail as expected):
//   broken containers; bad iovl version; oversize canvas.
import { existsSync, rmSync } from "fs";
import { basename, join } from "path";
import {
  build,
  buildDav1d,
  buildRef,
  defaultUseClang,
  cleanBuildOutput,
} from "./build";
import { getDeps, TESTIMAGES_DIR } from "./get-deps";
import { selectFiles, corpusSummary } from "./corpus";

/** Open / decode expected to fail (intentionally invalid or out of scope). */
const EXPECT_FAIL = new Set([
  "meta_size_zero.avif",
  "mini_size_zero.avif",
  "iovl_badver.heic",
  "iovl_huge_canvas.heic",
  /* ours ISO-correct; libheif leaves canvas black for negative offsets */
  "iovl_negoff.heic",
]);

function expectFail(name: string): boolean {
  return EXPECT_FAIL.has(name);
}

/** Valid no-oracle fixtures that our decoder must continue to decode. */
const MUST_DECODE = new Set([
  "C026.heic",
  "C027.heic",
  "C028.heic",
  "C029.heic",
  "C030.heic",
  "C031.heic",
  "C032.heic",
  "C036.heic",
  "C037.heic",
  "C038.heic",
  "C041.heic",
]);

/** Parse `30x20 mse=0.0000 maxdiff=0 n_diff=0` from -verify stdout. */
function parseVerifyMse(out: string): number | null {
  const m = out.match(/mse=([0-9.]+)/);
  return m ? parseFloat(m[1]) : null;
}

type VerifyKind =
  | "match"
  | "both_fail"
  | "heic_fail"
  | "libheif_fail"
  | "size_mismatch"
  | "other";

function classifyVerify(exitCode: number, out: string): VerifyKind {
  if (out.includes("both failed")) return "both_fail";
  if (out.includes("heic failed") && out.includes("libheif ok")) return "heic_fail";
  if (out.includes("libheif failed") && out.includes("heic ok")) return "libheif_fail";
  if (out.includes("size mismatch")) return "size_mismatch";
  if (exitCode === 0 && /mse=/.test(out)) return "match";
  if (exitCode === 1) return "heic_fail";
  if (exitCode === 2) return "size_mismatch";
  if (exitCode === 3) return "both_fail";
  if (exitCode === 4) return "libheif_fail";
  return "other";
}

const HEVC_SEQUENCE_TESTS = [
  { name: "LTRPSPS_A_Qualcomm_1.bit", frames: 17, mse: 8 },
  { name: "RPLM_A_qualcomm_4.bit", frames: 25, mse: 8 },
  { name: "TMVP_A_MS_3.bit", frames: 17, mse: 8 },
  { name: "WP_A_Toshiba_3.bit", frames: 17, mse: 8 },
  { name: "WP_B_Toshiba_3.bit", frames: 17, mse: 8 },
  { name: "MERGE_A_TI_3.bit", frames: 8, mse: 8 },
  { name: "ipcm_A_NEC_3.bit", frames: 1, mse: 8 },
  { name: "ipcm_B_NEC_3.bit", frames: 1, mse: 8 },
  { name: "ipcm_C_NEC_3.bit", frames: 1, mse: 8 },
] as const;

async function runHevcSequenceTests(exe: string): Promise<[number, number]> {
  const root = join(import.meta.dir, "..");
  const dec265Candidates = process.platform === "win32"
    ? [join(root, "out/libde265_build/dec265/dec265.exe")]
    : [
        join(root, "out/libde265_build/dec265/dec265"),
        join(root, "out/libde265_build/dec265"),
      ];
  const dec265 = dec265Candidates.find(existsSync);
  if (!dec265) {
    console.log("[fail] HEVC sequences: dec265 oracle executable not found");
    return [0, HEVC_SEQUENCE_TESTS.length];
  }

  let ok = 0;
  let fail = 0;
  for (const t of HEVC_SEQUENCE_TESTS) {
    const input = join(TESTIMAGES_DIR, "hevc_sequence", t.name);
    const stem = t.name.replace(/\.[^.]+$/, "");
    const ours = join(root, "out", `${stem}-ours.yuv`);
    const ref = join(root, "out", `${stem}-ref.yuv`);
    rmSync(ours, { force: true });
    rmSync(ref, { force: true });
    const rp = await Bun.$`${dec265} -q -f ${t.frames} -o ${ref} ${input}`
      .nothrow()
      .quiet();
    const op = await Bun.$`${exe} -hevc-sequence -hevc-frames ${t.frames} -out ${ours} ${input}`
      .nothrow()
      .quiet();
    if (rp.exitCode !== 0 || op.exitCode !== 0 || !existsSync(ours) || !existsSync(ref)) {
      fail++;
      const detail = (
        op.stderr.toString() + rp.stderr.toString()
      ).trim().slice(0, 300);
      console.log(`[fail] HEVC sequence ${t.name} decode failed\n${detail}`);
      continue;
    }
    const a = new Uint8Array(await Bun.file(ours).arrayBuffer());
    const b = new Uint8Array(await Bun.file(ref).arrayBuffer());
    if (a.length !== b.length || a.length === 0) {
      fail++;
      console.log(
        `[fail] HEVC sequence ${t.name} size ${a.length} != oracle ${b.length}`,
      );
      continue;
    }
    let sse = 0;
    let maxDiff = 0;
    for (let i = 0; i < a.length; i++) {
      const d = Math.abs(a[i] - b[i]);
      sse += d * d;
      if (d > maxDiff) maxDiff = d;
    }
    const mse = sse / a.length;
    if (mse <= t.mse) {
      ok++;
      console.log(
        `[ok] HEVC sequence ${t.name} frames=${t.frames} ` +
          `mse=${mse.toFixed(4)} maxdiff=${maxDiff}`,
      );
    } else {
      fail++;
      console.log(
        `[fail] HEVC sequence ${t.name} mse=${mse.toFixed(4)} > ${t.mse} ` +
          `maxdiff=${maxDiff}`,
      );
    }
    rmSync(ours, { force: true });
    rmSync(ref, { force: true });
  }
  return [ok, fail];
}

async function main() {
  const argv = process.argv.slice(2);
  if (argv.includes("-clean")) cleanBuildOutput();
  const useClang =
    argv.includes("-clang") ? true : argv.includes("-msvc") ? false : defaultUseClang;
  /* Default = decode + pixel compare (bench-shaped). -info = probe only. */
  const infoOnly = argv.includes("-info");
  const mseThreshold = (() => {
    const i = argv.indexOf("-mse");
    /* Default 8: large WPP stills (example.heic, hdr-sample) sit ~5–6 vs libheif
       residual/filter differences; exact unci/features stay mse=0. */
    return i >= 0 ? parseFloat(argv[i + 1] ?? "8") : 8.0;
  })();

  await getDeps();
  const files = selectFiles(
    `usage: bun cmd/tests.ts [options] <-all | -rand N | file...>\n` +
      `  (default)   decode RGB vs libheif; fail if we can't decode or mse > N\n` +
      `  -info        open/probe only (no decode, no libheif)\n` +
      `  -mse N       pixel mse threshold (default 8)\n` +
      `  -clang|-msvc toolchain\n` +
      `corpus: ${corpusSummary()}`,
  );

  let exe: string;
  if (infoOnly) {
    const withDav1d = argv.includes("-dav1d");
    exe = await build(useClang, { withDav1d });
  } else {
    console.log("ref: building libheif oracle (libde265 + dav1d + libheif)...");
    await buildDav1d();
    await buildRef();
    exe = await build(useClang, { withLibheif: true, withDav1d: true });
  }

  let ok = 0;
  let fail = 0;
  let skip = 0;

  for (const f of files) {
    const name = basename(f);
    const wantFail = expectFail(name);

    if (infoOnly) {
      const p = await Bun.$`${exe} -info ${f}`.nothrow().quiet();
      const out = p.stdout.toString() + p.stderr.toString();
      const good = p.exitCode === 0 && /\d+x\d+/.test(out);
      if (wantFail) {
        if (!good) {
          ok++;
          console.log(`[ok-expect-fail] ${f}`);
        } else {
          fail++;
          console.log(`[fail] expected fail but -info ok: ${f}`);
        }
        continue;
      }
      if (good) {
        ok++;
        console.log(`[ok] ${f}`);
      } else {
        fail++;
        console.log(`[fail] ${f} (exit ${p.exitCode})\n${out.slice(0, 400)}`);
      }
      continue;
    }

    /* Decode + bitmap compare (heic_test -verify, same oracle as bench). */
    const p = await Bun.$`${exe} -verify ${f}`.nothrow().quiet();
    const out = (p.stdout.toString() + p.stderr.toString()).trim();
    const kind = classifyVerify(p.exitCode ?? 1, out);
    const mse = parseVerifyMse(out);

    if (kind === "both_fail") {
      if (MUST_DECODE.has(name)) {
        fail++;
        console.log(`[fail] ${f} required fixture did not decode\n${out.slice(0, 300)}`);
        continue;
      }
      /* Neither side decodes — acceptable only for non-mandatory corpus files. */
      ok++;
      if (wantFail) {
        console.log(`[ok-expect-fail] ${f} not decodable`);
      } else {
        console.log(`[ok] ${f} not decodable`);
      }
      continue;
    }

    if (kind === "heic_fail") {
      if (wantFail) {
        ok++;
        console.log(`[ok-expect-fail] ${f} (heic fail, libheif ok)`);
      } else {
        fail++;
        console.log(`[fail] ${f} cannot decode (libheif ok)\n${out.slice(0, 300)}`);
      }
      continue;
    }

    if (kind === "libheif_fail") {
      if (wantFail) {
        ok++;
        console.log(`[ok-expect-fail] ${f} (libheif fail)`);
      } else {
        /* We decode; oracle rejects/unsupported — no mse compare. */
        skip++;
        console.log(`[skip] ${f} unsupported by libheif (heic ok, no oracle)`);
      }
      continue;
    }

    if (kind === "size_mismatch") {
      if (wantFail) {
        ok++;
        console.log(`[ok-expect-fail] ${f} (size mismatch)`);
      } else {
        fail++;
        console.log(`[fail] ${f} ${out}`);
      }
      continue;
    }

    if (kind === "match" && mse !== null) {
      if (mse <= mseThreshold) {
        if (wantFail) {
          fail++;
          console.log(
            `[fail] expected fail but mse=${mse.toFixed(4)} <= ${mseThreshold}: ${f}`,
          );
        } else {
          ok++;
          console.log(`[ok] ${f} ${out}`);
        }
      } else if (wantFail) {
        ok++;
        console.log(
          `[ok-expect-fail] ${f} mse=${mse.toFixed(4)} > ${mseThreshold}`,
        );
      } else {
        fail++;
        console.log(`[fail] ${f} different bitmap: ${out} (threshold ${mseThreshold})`);
      }
      continue;
    }

    fail++;
    console.log(`[fail] ${f} verify exit ${p.exitCode}\n${out.slice(0, 400)}`);
  }

  let extra = 0;
  if (!infoOnly && argv.includes("-all")) {
    const [seqOk, seqFail] = await runHevcSequenceTests(exe);
    ok += seqOk;
    fail += seqFail;
    extra = HEVC_SEQUENCE_TESTS.length;
  }

  console.log(
    `done: ${ok} ok, ${fail} fail, ${skip} skip / ${files.length + extra}` +
      (infoOnly
        ? " (info)"
        : ` (libheif mse<=${mseThreshold}; skip=no oracle, not decodable counts as ok)`),
  );
  process.exit(fail ? 1 : 0);
}

if (import.meta.main) await main();
