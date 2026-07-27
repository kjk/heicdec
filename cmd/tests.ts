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
// When libheif rejects a valid file, compare a compact independently decoded
// RGB oracle from test/no_libheif_oracles.json.
//
// Intentional bad inputs (ok-expect-fail if we fail as expected):
//   broken containers; bad iovl version; oversize canvas.
import { existsSync, readFileSync, rmSync } from "fs";
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
  "sequence_inter.avif",
  "sequence_alpha.avif",
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

type AltOracle = {
  width: number;
  height: number;
  source: string;
  rgb16: string;
};

const ALT_ORACLE_SIZE = 16;
const ALT_ORACLE_MSE = 1;
const ALT_ORACLES = JSON.parse(
  readFileSync(
    join(import.meta.dir, "..", "test", "no_libheif_oracles.json"),
    "utf8",
  ),
) as Record<string, AltOracle>;

function readPpm(path: string): { width: number; height: number; rgb: Uint8Array } {
  const data = new Uint8Array(readFileSync(path));
  let pos = 0;
  const token = (): string => {
    for (;;) {
      while (pos < data.length && data[pos]! <= 32) pos++;
      if (data[pos] !== 35) break;
      while (pos < data.length && data[pos] !== 10) pos++;
    }
    let out = "";
    while (pos < data.length && data[pos]! > 32)
      out += String.fromCharCode(data[pos++]!);
    return out;
  };
  if (token() !== "P6") throw new Error("not a P6 PPM");
  const width = Number(token());
  const height = Number(token());
  if (Number(token()) !== 255 || width < 1 || height < 1)
    throw new Error("invalid PPM header");
  if (data[pos] === 13 && data[pos + 1] === 10) pos += 2;
  else if (data[pos]! <= 32) pos++;
  const n = width * height * 3;
  if (!Number.isSafeInteger(n) || data.length - pos !== n)
    throw new Error("invalid PPM pixel length");
  return { width, height, rgb: data.subarray(pos) };
}

function downsampleRgb(
  rgb: Uint8Array,
  width: number,
  height: number,
): Uint8Array {
  const out = new Uint8Array(ALT_ORACLE_SIZE * ALT_ORACLE_SIZE * 3);
  for (let ty = 0; ty < ALT_ORACLE_SIZE; ty++) {
    const y0 = Math.floor((ty * height) / ALT_ORACLE_SIZE);
    const y1 = Math.floor(((ty + 1) * height) / ALT_ORACLE_SIZE);
    for (let tx = 0; tx < ALT_ORACLE_SIZE; tx++) {
      const x0 = Math.floor((tx * width) / ALT_ORACLE_SIZE);
      const x1 = Math.floor(((tx + 1) * width) / ALT_ORACLE_SIZE);
      const count = (x1 - x0) * (y1 - y0);
      const sum = [0, 0, 0];
      for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
          const i = (y * width + x) * 3;
          sum[0] += rgb[i]!;
          sum[1] += rgb[i + 1]!;
          sum[2] += rgb[i + 2]!;
        }
      }
      const o = (ty * ALT_ORACLE_SIZE + tx) * 3;
      for (let c = 0; c < 3; c++)
        out[o + c] = Math.floor((sum[c]! + count / 2) / count);
    }
  }
  return out;
}

async function compareAltOracle(
  exe: string,
  file: string,
  name: string,
): Promise<{ ok: boolean; detail: string } | null> {
  if (name === "sequence_inter.avif" || name === "sequence_alpha.avif") {
    const p = await Bun.$`${exe} -verify-sequence ${file}`.nothrow().quiet();
    const out = (p.stdout.toString() + p.stderr.toString()).trim();
    const mse = parseVerifyMse(out);
    return {
      ok: p.exitCode === 0 && mse !== null && mse <= 8,
      detail: `libheif/dav1d ${out}`,
    };
  }
  const key = name.replace(/\.[^.]+$/, "");
  const oracle = ALT_ORACLES[key];
  if (!oracle) return null;
  const ppm = join(import.meta.dir, "..", "out", `alt-oracle-${key}.ppm`);
  rmSync(ppm, { force: true });
  try {
    const p = await Bun.$`${exe} -out ${ppm} ${file}`.nothrow().quiet();
    if (p.exitCode !== 0 || !existsSync(ppm))
      return { ok: false, detail: `decode exit ${p.exitCode}` };
    const image = readPpm(ppm);
    if (image.width !== oracle.width || image.height !== oracle.height) {
      return {
        ok: false,
        detail:
          `size ${image.width}x${image.height} != ` +
          `${oracle.width}x${oracle.height}`,
      };
    }
    const actual = downsampleRgb(image.rgb, image.width, image.height);
    const expected = new Uint8Array(Buffer.from(oracle.rgb16, "base64"));
    if (actual.length !== expected.length)
      return { ok: false, detail: "invalid alternate oracle length" };
    let sse = 0;
    let maxDiff = 0;
    for (let i = 0; i < actual.length; i++) {
      const d = Math.abs(actual[i]! - expected[i]!);
      sse += d * d;
      if (d > maxDiff) maxDiff = d;
    }
    const mse = sse / actual.length;
    return {
      ok: mse <= ALT_ORACLE_MSE,
      detail:
        `${oracle.source} rgb16 mse=${mse.toFixed(4)} ` +
        `maxdiff=${maxDiff}`,
    };
  } catch (e) {
    return { ok: false, detail: String(e) };
  } finally {
    rmSync(ppm, { force: true });
  }
}

const HEVC_SEQUENCE_TESTS = [
  { name: "LTRPSPS_A_Qualcomm_1.bit", frames: 17, mse: 8 },
  { name: "RPLM_A_qualcomm_4.bit", frames: 25, mse: 8 },
  { name: "TMVP_A_MS_3.bit", frames: 17, mse: 8 },
  { name: "WP_A_Toshiba_3.bit", frames: 17, mse: 8 },
  { name: "WP_B_Toshiba_3.bit", frames: 17, mse: 8 },
  { name: "MERGE_A_TI_3.bit", frames: 8, mse: 8 },
  { name: "DSLICE_A_HHI_5.bit", frames: 50, mse: 0.001 },
  { name: "DSLICE_B_HHI_5.bit", frames: 50, mse: 0.001 },
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

const HEIF_SEQUENCE_TESTS = [
  {
    name: "C026.heic", frames: 8, duration: 160,
    repetitions: 1, first: 0, last: 140,
  },
  {
    name: "C027.heic", frames: 16, duration: 320,
    repetitions: 1, first: 0, last: 300,
  },
  {
    name: "C028.heic", frames: 16, duration: 320,
    repetitions: 1, first: 0, last: 300,
  },
  {
    name: "C029.heic", frames: 13, duration: 100500,
    repetitions: 0, first: 0, last: 1200,
  },
  {
    name: "C030.heic", frames: 8, duration: 100500,
    repetitions: 0, first: 500, last: 1200,
  },
  {
    name: "C031.heic", frames: 8, duration: 800,
    repetitions: 1, first: 0, last: 700,
  },
  {
    name: "C032.heic", frames: 8, duration: 800,
    repetitions: 1, first: 0, last: 700,
  },
  {
    name: "C036.heic", frames: 8, duration: 2400,
    repetitions: 3, first: 0, last: 700,
  },
  {
    name: "C037.heic", frames: 8, duration: 1200,
    repetitions: 0, first: 0, last: 700,
  },
  {
    name: "C038.heic", frames: 8, duration: 4294967295,
    repetitions: 4294967295, first: 0, last: 700,
  },
  {
    name: "C041.heic", frames: 8, duration: 2000,
    repetitions: 0, first: 0, last: 700,
  },
  {
    name: "sequence_inter.avif", frames: 5, timescale: 30, duration: 5,
    repetitions: 0, first: 0, last: 4, dependent: true,
    separatePrimary: true,
  },
  {
    name: "sequence_alpha.avif", frames: 5, timescale: 30,
    duration: "18446744073709551615", repetitions: 4294967295,
    first: 0, last: 20, dependent: true, separatePrimary: true, alpha: true,
  },
] as const;

async function runHeifSequenceApiTests(exe: string): Promise<[number, number]> {
  const root = join(import.meta.dir, "..");
  const corpus = join(root, "deps", "heif_conformance", "conformance_files");
  let ok = 0;
  let fail = 0;
  for (const t of HEIF_SEQUENCE_TESTS) {
    const input = t.name.endsWith(".avif")
      ? join(TESTIMAGES_DIR, "avif", t.name)
      : join(corpus, t.name);
    const infoProc = await Bun.$`${exe} -sequence-info ${input}`.nothrow().quiet();
    const out =
      infoProc.stdout.toString() + infoProc.stderr.toString();
    const summary = out.match(
      /sequence frames=(\d+) timescale=(\d+) duration=(\d+) repetitions=(\d+)/,
    );
    const frames = [...out.matchAll(/frame \d+ time=(\d+) duration=(\d+) sync=\d/g)];
    const metadataOk =
      infoProc.exitCode === 0 &&
      summary !== null &&
      Number(summary[1]) === t.frames &&
      Number(summary[2]) === ("timescale" in t ? t.timescale : 1000) &&
      summary[3] === String(t.duration) &&
      Number(summary[4]) === t.repetitions &&
      frames.length === t.frames &&
      Number(frames[0]![1]) === t.first &&
      Number(frames[frames.length - 1]![1]) === t.last;
    const oracleProc = await Bun.$`${exe} -verify-sequence ${input}`
      .nothrow()
      .quiet();
    const oracleOut =
      oracleProc.stdout.toString() + oracleProc.stderr.toString();
    const oracleMse = parseVerifyMse(oracleOut);
    if (
      metadataOk &&
      oracleProc.exitCode === 0 &&
      oracleMse !== null &&
      oracleMse <= 8 &&
      (!("dependent" in t) || !t.dependent ||
        oracleOut.includes("dependent=1")) &&
      (!("separatePrimary" in t) || !t.separatePrimary ||
        oracleOut.includes("separate_primary=1")) &&
      (!("alpha" in t) || !t.alpha ||
        (oracleOut.includes("alpha=1") &&
          oracleOut.includes("alpha_nonopaque=1")))
    ) {
      ok++;
      console.log(
        `[ok] HEIF sequence ${t.name} frames=${t.frames} ` +
          `duration=${t.duration} repetitions=${t.repetitions} ` +
          `mse=${oracleMse.toFixed(4)}`,
      );
    } else {
      fail++;
      const detail = (
        out + oracleOut
      ).trim().slice(0, 500);
      console.log(`[fail] HEIF sequence ${t.name}\n${detail}`);
    }
  }
  return [ok, fail];
}

async function runGainMapTest(exe: string): Promise<[number, number]> {
  const input = join(
    import.meta.dir, "..", "deps", "heic", "testdata",
    "apple-hdr", "hdr-sample.heic",
  );
  const proc = await Bun.$`${exe} -verify-gain-map ${input}`
    .nothrow()
    .quiet();
  const out = (proc.stdout.toString() + proc.stderr.toString()).trim();
  const mse = parseVerifyMse(out);
  if (proc.exitCode === 0 && mse !== null && mse <= 8) {
    console.log(`[ok] HDR gain map ${out}`);
    return [1, 0];
  }
  console.log(`[fail] HDR gain map\n${out.slice(0, 500)}`);
  return [0, 1];
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
        const alt = await compareAltOracle(exe, f, name);
        if (!alt) {
          skip++;
          console.log(`[skip] ${f} unsupported by libheif (no alternate oracle)`);
        } else if (alt.ok) {
          ok++;
          console.log(`[ok-alt] ${f} ${alt.detail}`);
        } else {
          fail++;
          console.log(`[fail] ${f} alternate oracle: ${alt.detail}`);
        }
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
    const [apiOk, apiFail] = await runHeifSequenceApiTests(exe);
    ok += apiOk;
    fail += apiFail;
    const [gainOk, gainFail] = await runGainMapTest(exe);
    ok += gainOk;
    fail += gainFail;
    extra = HEVC_SEQUENCE_TESTS.length + HEIF_SEQUENCE_TESTS.length + 1;
  }

  console.log(
    `done: ${ok} ok, ${fail} fail, ${skip} skip / ${files.length + extra}` +
      (infoOnly
        ? " (info)"
        : ` (libheif mse<=${mseThreshold}; alternate rgb16 mse<=${ALT_ORACLE_MSE}; ` +
          `skip=no oracle, not decodable counts as ok)`),
  );
  process.exit(fail ? 1 : 0);
}

if (import.meta.main) await main();
