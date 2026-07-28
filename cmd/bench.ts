// bench.ts -- benchmark our decoder against strukturag libheif (native).
// On Windows also times the OS WIC HEIF/AVIF codec (time only).
//
//   bun cmd/bench.ts <file.heic ... | -rand N | -all> [-verbose] [-clang] [-clean]
//   bun cmd/bench.ts -list-files
//
// Builds the oracle stack (libde265 + dav1d + libheif) and heic_test with
// -libheif, then runs `heic_test -bench` on each selected file. Session
// benchmark: open from memory, decode primary RGB, close; best of 3
// interleaved runs each side.
//
// Default output: directory header lines (`deps/...`), then per-file totals
//   libheif heic [wic] diff %diff <basename> : <bytes>
//   (wic column on Windows only; time only — no wic vs heic/libheif diff)
// Last data line is sum of best-of-3 totals, label "total". After that:
// wall-clock elapsed and the top 10 files where heic is slowest vs libheif
// (by relative %; files with |diff| < 2ms omitted). `-verbose` also prints
// open/decode/close comparisons per file.
//
// With no selection it prints usage + the available corpus file count.
import { basename, dirname } from "path";
import { getDeps } from "./get-deps";
import { build, buildRef, cleanBuildOutput, defaultUseClang, isWindows } from "./build";
import {
  corpusFiles,
  corpusSummary,
  fileLabel,
  fileNameLabel,
  fileRel,
  selectFiles,
} from "./corpus";

const ROOT = dirname(import.meta.dir).replaceAll("\\", "/");

/** Print `deps/...` dir once when it changes; return basename(+size) for the line. */
function enterDirAndName(
  file: string,
  lastDir: { value: string },
): string {
  const rel = fileRel(file, ROOT);
  const dir = dirname(rel).replaceAll("\\", "/");
  if (dir !== lastDir.value) {
    console.log(dir);
    lastDir.value = dir;
  }
  try {
    return fileNameLabel(file);
  } catch {
    return basename(file);
  }
}

/**
 * Intentionally invalid fixtures (malformed / oversize). Both heic and libheif
 * reject them; do not treat as decoder failures in the bench table.
 * Keep in sync with EXPECT_FAIL decode-rejects in tests.ts.
 */
const BENCH_EXPECT_REJECT = new Set([
  "meta_size_zero.avif",
  "mini_size_zero.avif",
  "iovl_badver.heic",
  "iovl_huge_canvas.heic",
]);

type BenchTimes = {
  open: number;
  decode: number;
  close: number;
  total: number;
};

type BenchResult = {
  oursOk: boolean;
  libheifOk: boolean;
  ours: BenchTimes;
  libheif: BenchTimes;
  libheifError: string;
  /** Present on Windows builds; null when missing / not reported. */
  wicOk: boolean | null;
  wicTotal: number | null;
  wicError: string;
};

type RankedResult = {
  label: string;
  ours: number;
  libheif: number;
  wic: number | null;
  diff: number;
  pct: number;
};

function parseBenchResult(out: string): BenchResult | null {
  const line = out.split(/\r?\n/).find((s) => s.startsWith("BENCH_RESULT "));
  if (!line) return null;
  const fields = new Map<string, string>();
  for (const part of line.slice("BENCH_RESULT ".length).trim().split(/\s+/)) {
    const eq = part.indexOf("=");
    if (eq > 0) fields.set(part.slice(0, eq), part.slice(eq + 1));
  }
  const num = (key: string): number => Number(fields.get(key) ?? "NaN");
  const hasWic = fields.has("wic_ok") || fields.has("wic_total");
  const wicOkFlag = fields.get("wic_ok");
  const wicTotalRaw = num("wic_total");
  const result: BenchResult = {
    oursOk: fields.get("ours_ok") === "1",
    libheifOk: fields.get("libheif_ok") === "1",
    ours: {
      open: num("ours_open"),
      decode: num("ours_decode"),
      close: num("ours_close"),
      total: num("ours_total"),
    },
    libheif: {
      open: num("libheif_open"),
      decode: num("libheif_decode"),
      close: num("libheif_close"),
      total: num("libheif_total"),
    },
    libheifError:
      out.match(/^BENCH_LIBHEIF_ERROR (.*)$/m)?.[1]?.trim() || "unknown error",
    wicOk: hasWic ? wicOkFlag === "1" : null,
    wicTotal: hasWic && Number.isFinite(wicTotalRaw) && wicTotalRaw >= 0
      ? wicTotalRaw
      : hasWic
        ? null
        : null,
    wicError:
      out.match(/^BENCH_WIC_ERROR (.*)$/m)?.[1]?.trim() || "unknown error",
  };
  return Number.isFinite(result.ours.total) &&
    Number.isFinite(result.libheif.total)
    ? result
    : null;
}

function fmtMs(n: number | null): string {
  return n === null ? "ERROR" : n.toFixed(2);
}

function fmtDiff(ours: number | null, lib: number | null): string {
  if (ours === null || lib === null) return "ERROR";
  const d = ours - lib;
  return `${d >= 0 ? "+" : ""}${d.toFixed(2)}`;
}

function fmtPct(ours: number | null, lib: number | null): string {
  if (ours === null || lib === null) return "ERROR";
  if (lib > 0) {
    const p = ((ours - lib) / lib) * 100;
    return `${p >= 0 ? "+" : ""}${p.toFixed(1)}%`;
  }
  return "0.0%";
}

// Compact default line: right-aligned number columns, then file.
const col = (s: string) => s.padStart(8);

/** libheif / heic / [wic] / diff / %diff / file. WIC is Windows-only, time only. */
function printCompactLine(
  lib: string,
  ours: string,
  wic: string | null,
  diff: string,
  pct: string,
  label: string,
): void {
  if (wic !== null) {
    console.log(
      `${col(lib)} ${col(ours)} ${col(wic)} ${col(diff)} ${col(pct)} ${label}`,
    );
  } else {
    console.log(`${col(lib)} ${col(ours)} ${col(diff)} ${col(pct)} ${label}`);
  }
}

function printHeader(withWic: boolean): void {
  if (withWic) printCompactLine("libheif", "heic", "wic", "diff", "%diff", "file");
  else printCompactLine("libheif", "heic", null, "diff", "%diff", "file");
}

function wicCell(result: BenchResult | null, forceSkip = false): string | null {
  if (!isWindows) return null;
  if (forceSkip || !result) return "SKIP";
  if (result.wicOk === null) return "—";
  if (!result.wicOk || result.wicTotal === null) return "SKIP";
  return fmtMs(result.wicTotal);
}

function signed(value: number, digits: number): string {
  return `${value >= 0 ? "+" : ""}${value.toFixed(digits)}`;
}

function comparisonLine(op: string, ours: number, libheif: number): string {
  const diff = ours - libheif;
  const pct = libheif > 0 ? (diff / libheif) * 100 : 0;
  return `${op}: libheif ${libheif.toFixed(2)}ms -> heic ${ours.toFixed(2)}ms, ` +
    `${signed(diff, 2)}ms (${signed(pct, 1)}%)`;
}

function formatElapsed(elapsedMs: number): string {
  let remaining = Math.max(0, Math.round(elapsedMs));
  const hours = Math.floor(remaining / 3_600_000);
  remaining %= 3_600_000;
  const minutes = Math.floor(remaining / 60_000);
  remaining %= 60_000;
  const seconds = Math.floor(remaining / 1000);
  const milliseconds = remaining % 1000;
  const parts: string[] = [];
  if (hours) parts.push(`${hours}h`);
  if (minutes) parts.push(`${minutes}m`);
  if (seconds) parts.push(`${seconds}s`);
  parts.push(`${milliseconds}ms`);
  return parts.join(" ");
}

async function main(): Promise<void> {
  const startedAt = performance.now();
  const argv = process.argv.slice(2);
  const useClang =
    argv.includes("-clang") ? true : argv.includes("-msvc") ? false : defaultUseClang;
  const doClean = argv.includes("-clean");
  const verbose = argv.includes("-verbose");
  const withWic = isWindows;

  await getDeps();

  if (argv.includes("-list-files")) {
    const all = corpusFiles();
    const lastDir = { value: "" };
    for (const f of all) {
      const name = enterDirAndName(f, lastDir);
      console.log(name);
    }
    console.log(`\n${all.length} file(s)`);
    process.exit(0);
  }

  const files = selectFiles(
    `usage: bun cmd/bench.ts <selection> [options]
selection (required; default prints this help):
  file.heic ...   bench the given files
  -rand N         bench N randomly selected corpus files
  -all            bench every corpus file
  -list-files     list corpus dirs + basenames (with size) and exit
options:
  -verbose        also print open/decode/close timing changes
  -clang          build with clang instead of MSVC
  -clean          delete out/ first (forces full rebuild of harness + oracle)

Default: dir headers (deps/...), then basename lines
  libheif heic [wic] diff %diff file
  (wic on Windows only = OS Imaging Component time; diff/%diff = heic vs libheif)
ends with a "total" line (sum of best-of-3 times), then elapsed and top 10
slowest vs libheif (+ = heic slower; |diff| < 2ms ignored).

Oracle: strukturag libheif + libde265 (+ dav1d), built static via cmake/ninja
into out/libheif_build (same idea as djvudec's libdjvu static oracle).

${corpusSummary()}`,
    ["-rand"],
  );

  if (doClean) {
    console.log("clean: removing out/...");
    cleanBuildOutput();
  }

  console.log("ref: building libheif oracle (libde265 + dav1d + libheif)...");
  await buildRef();
  const exe = await build(useClang, { withLibheif: true, withDav1d: true });
  console.log(`harness: ${exe}`);

  let rc = 0;
  let n_ok = 0;
  let n_skip = 0;
  let n_fail = 0;
  const ranked: RankedResult[] = [];

  if (!verbose) printHeader(withWic);

  const lastDir = { value: "" };
  for (const file of files) {
    const nameLabel = enterDirAndName(file, lastDir);
    const rankLabel = (() => {
      try {
        return fileLabel(file, ROOT);
      } catch {
        return fileRel(file, ROOT);
      }
    })();
    if (verbose) console.log(`=== ${nameLabel}`);
    const r = Bun.spawnSync({
      cmd: [exe, "-bench", file],
      stdout: "pipe",
      stderr: "pipe",
      cwd: ROOT,
    });
    const out = (r.stdout?.toString() ?? "") + (r.stderr?.toString() ?? "");
    const result = parseBenchResult(out);
    const name = basename(file);
    const expectReject = BENCH_EXPECT_REJECT.has(name);

    if (!result) {
      if (expectReject) {
        if (verbose) console.log(`total: skip expected reject ${name}`);
        else printCompactLine("SKIP", "SKIP", wicCell(null, true), "SKIP", "SKIP", nameLabel);
        n_skip++;
        continue;
      }
      if (verbose) {
        console.log(`total: benchmark failed (exit ${r.exitCode ?? "unknown"})`);
        if (out.trim()) console.log(out.trim());
      } else {
        printCompactLine("ERROR", "ERROR", withWic ? "ERROR" : null, "ERROR", "ERROR", nameLabel);
      }
      n_fail++;
      rc = r.exitCode || 1;
      continue;
    }

    if (result.oursOk && result.libheifOk) {
      if (verbose) {
        console.log(comparisonLine("total", result.ours.total, result.libheif.total));
        console.log(comparisonLine("open", result.ours.open, result.libheif.open));
        console.log(comparisonLine("decode", result.ours.decode, result.libheif.decode));
        console.log(comparisonLine("close", result.ours.close, result.libheif.close));
        if (withWic) {
          if (result.wicOk && result.wicTotal !== null)
            console.log(`wic: ${result.wicTotal.toFixed(2)}ms`);
          else
            console.log(`wic: SKIP (${result.wicError})`);
        }
      } else {
        printCompactLine(
          fmtMs(result.libheif.total),
          fmtMs(result.ours.total),
          wicCell(result),
          fmtDiff(result.ours.total, result.libheif.total),
          fmtPct(result.ours.total, result.libheif.total),
          nameLabel,
        );
      }
      const diff = result.ours.total - result.libheif.total;
      ranked.push({
        label: rankLabel,
        ours: result.ours.total,
        libheif: result.libheif.total,
        wic: result.wicOk && result.wicTotal !== null ? result.wicTotal : null,
        diff,
        pct: result.libheif.total > 0 ? (diff / result.libheif.total) * 100 : 0,
      });
      n_ok++;
      continue;
    }

    if (!result.libheifOk) {
      /* Both reject (or libheif-only): skip compare. Intentional rejects are
       * expected; other both-fail is still a skip (unsupported / no oracle). */
      if (verbose) {
        const ours = result.oursOk
          ? `; heic ${result.ours.total.toFixed(2)}ms`
          : "; heic failed";
        const tag = expectReject ? "expected reject" : "libheif failed";
        console.log(`total: ${tag}: ${result.libheifError}${ours}`);
        if (withWic) {
          if (result.wicOk && result.wicTotal !== null)
            console.log(`wic: ${result.wicTotal.toFixed(2)}ms`);
          else
            console.log(`wic: SKIP (${result.wicError})`);
        }
      } else if (!result.oursOk) {
        printCompactLine("SKIP", "SKIP", wicCell(result), "SKIP", "SKIP", nameLabel);
      } else {
        printCompactLine(
          "SKIP",
          fmtMs(result.ours.total),
          wicCell(result),
          "SKIP",
          "SKIP",
          nameLabel,
        );
      }
      n_skip++;
      continue;
    }

    /* libheif ok, heic failed — real decoder regression (unless expected reject). */
    if (expectReject) {
      if (verbose) console.log(`total: expected reject (heic fail, libheif ok?)`);
      else printCompactLine("SKIP", "SKIP", wicCell(result), "SKIP", "SKIP", nameLabel);
      n_skip++;
      continue;
    }
    if (verbose) {
      console.log(`total: libheif ${result.libheif.total.toFixed(2)}ms; heic failed`);
      if (withWic) {
        if (result.wicOk && result.wicTotal !== null)
          console.log(`wic: ${result.wicTotal.toFixed(2)}ms`);
        else
          console.log(`wic: SKIP (${result.wicError})`);
      }
    } else {
      printCompactLine(
        fmtMs(result.libheif.total),
        "ERROR",
        wicCell(result),
        "ERROR",
        "ERROR",
        nameLabel,
      );
    }
    n_fail++;
    rc = r.exitCode || 1;
  }

  if (ranked.length > 0) {
    const sumLib = ranked.reduce((s, r) => s + r.libheif, 0);
    const sumOurs = ranked.reduce((s, r) => s + r.ours, 0);
    const sumWic = ranked.reduce(
      (s, r) => (r.wic !== null ? (s === null ? r.wic : s + r.wic) : s),
      null as number | null,
    );
    printCompactLine(
      fmtMs(sumLib),
      fmtMs(sumOurs),
      withWic ? (sumWic === null ? "—" : fmtMs(sumWic)) : null,
      fmtDiff(sumOurs, sumLib),
      fmtPct(sumOurs, sumLib),
      "total",
    );
  }

  console.log(`\nbench summary: ok=${n_ok} skip=${n_skip} fail=${n_fail}`);
  console.log(`elapsed: ${formatElapsed(performance.now() - startedAt)}`);
  console.log(
    "top 10 slowest relative to libheif (+ = heic slower; |diff| < 2ms ignored):",
  );
  const slowest = ranked
    .filter((r) => Math.abs(r.diff) >= 2)
    .sort((a, b) => b.pct - a.pct || b.diff - a.diff)
    .slice(0, 10);
  if (slowest.length === 0) {
    console.log("  no comparable files");
  } else {
    for (const [index, item] of slowest.entries()) {
      const wicPart =
        withWic && item.wic !== null
          ? ` | wic ${item.wic.toFixed(2)}ms`
          : withWic
            ? " | wic —"
            : "";
      console.log(
        `${index + 1}. ${signed(item.pct, 1)}% | heic ${item.ours.toFixed(2)}ms | ` +
          `libheif ${item.libheif.toFixed(2)}ms${wicPart} | ${item.label}`,
      );
    }
  }
  process.exit(rc);
}

if (import.meta.main) await main();
