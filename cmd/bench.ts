// bench.ts -- benchmark our decoder against strukturag libheif (native).
//
//   bun cmd/bench.ts <file.heic ... | -rand N | -all> [-verbose] [-clang] [-clean]
//   bun cmd/bench.ts -list-files
//
// Builds the oracle stack (libde265 + dav1d + libheif) and heic_test with
// -libheif, then runs `heic_test -bench` on each selected file. Session
// benchmark: open from memory, decode primary RGB, close; best of 3
// interleaved runs each side.
//
// Default output: one header line, then one total line per file
//   libheif heic diff %diff file
// After all files: wall-clock elapsed and the top 10 files where heic is
// slowest vs libheif (by relative %). `-verbose` also prints open/decode/close
// comparisons per file.
//
// With no selection it prints usage + the available corpus file count.
import { dirname } from "path";
import { getDeps } from "./get-deps";
import { build, buildRef, cleanBuildOutput, defaultUseClang } from "./build";
import { corpusFiles, corpusSummary, fileLabel, selectFiles } from "./corpus";

const ROOT = dirname(import.meta.dir).replaceAll("\\", "/");

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
};

type RankedResult = {
  label: string;
  ours: number;
  libheif: number;
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

// Compact default line: 4× 8-char right-aligned number columns, then file.
const col = (s: string) => s.padStart(8);
function printCompactLine(
  lib: string,
  ours: string,
  diff: string,
  pct: string,
  label: string,
): void {
  console.log(`${col(lib)} ${col(ours)} ${col(diff)} ${col(pct)} ${label}`);
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

  await getDeps();

  if (argv.includes("-list-files")) {
    const all = corpusFiles();
    for (const f of all) {
      try {
        console.log(fileLabel(f, ROOT));
      } catch {
        console.log(f);
      }
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
  -list-files     list corpus files (path, size) and exit
options:
  -verbose        also print open/decode/close timing changes
  -clang          build with clang instead of MSVC
  -clean          delete out/ first (forces full rebuild of harness + oracle)

Default: one header + one total line per file (libheif heic diff %diff file),
then elapsed and top 10 slowest vs libheif (+ = heic slower).

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

  if (!verbose) printCompactLine("libheif", "heic", "diff", "%diff", "file");

  for (const file of files) {
    const label = fileLabel(file, ROOT);
    if (verbose) console.log(`=== ${label}`);
    const r = Bun.spawnSync({
      cmd: [exe, "-bench", file],
      stdout: "pipe",
      stderr: "pipe",
      cwd: ROOT,
    });
    const out = (r.stdout?.toString() ?? "") + (r.stderr?.toString() ?? "");
    const result = parseBenchResult(out);
    if (!result) {
      if (verbose) {
        console.log(`total: benchmark failed (exit ${r.exitCode ?? "unknown"})`);
        if (out.trim()) console.log(out.trim());
      } else {
        printCompactLine("ERROR", "ERROR", "ERROR", "ERROR", label);
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
      } else {
        printCompactLine(
          fmtMs(result.libheif.total),
          fmtMs(result.ours.total),
          fmtDiff(result.ours.total, result.libheif.total),
          fmtPct(result.ours.total, result.libheif.total),
          label,
        );
      }
      const diff = result.ours.total - result.libheif.total;
      ranked.push({
        label,
        ours: result.ours.total,
        libheif: result.libheif.total,
        diff,
        pct: result.libheif.total > 0 ? (diff / result.libheif.total) * 100 : 0,
      });
      n_ok++;
      continue;
    }

    if (!result.libheifOk) {
      if (verbose) {
        const ours = result.oursOk
          ? `; heic ${result.ours.total.toFixed(2)}ms`
          : "; heic failed";
        console.log(`total: libheif failed: ${result.libheifError}${ours}`);
      } else {
        printCompactLine(
          "ERROR",
          result.oursOk ? fmtMs(result.ours.total) : "ERROR",
          "ERROR",
          "ERROR",
          label,
        );
      }
      n_skip++;
      continue;
    }

    if (verbose) {
      console.log(`total: libheif ${result.libheif.total.toFixed(2)}ms; heic failed`);
    } else {
      printCompactLine(
        fmtMs(result.libheif.total),
        "ERROR",
        "ERROR",
        "ERROR",
        label,
      );
    }
    n_fail++;
    rc = r.exitCode || 1;
  }

  console.log(`\nbench summary: ok=${n_ok} skip=${n_skip} fail=${n_fail}`);
  console.log(`elapsed: ${formatElapsed(performance.now() - startedAt)}`);
  console.log("top 10 slowest relative to libheif (+ = heic slower):");
  const slowest = ranked.sort((a, b) => b.pct - a.pct || b.diff - a.diff).slice(0, 10);
  if (slowest.length === 0) {
    console.log("  no comparable files");
  } else {
    for (const [index, item] of slowest.entries()) {
      console.log(
        `${index + 1}. ${signed(item.pct, 1)}% | heic ${item.ours.toFixed(2)}ms | ` +
          `libheif ${item.libheif.toFixed(2)}ms | ${item.label}`,
      );
    }
  }
  process.exit(rc);
}

if (import.meta.main) await main();
