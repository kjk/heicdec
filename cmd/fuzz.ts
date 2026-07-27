// fuzz.ts -- coverage-guided fuzzing of the HEIC/HEIF/AVIF decoder (libFuzzer).
//
//   bun cmd/fuzz.ts             build, seed corpus if empty, fuzz until killed
//   bun cmd/fuzz.ts -jobs 8     run 8 parallel workers sharing the corpus
//   bun cmd/fuzz.ts -repro F    replay a single crash artifact and exit
//   bun cmd/fuzz.ts -minimize   shrink the corpus to a minimal covering set
//
// The fuzzer is always rebuilt from scratch: buildFuzz tracks sources + public
// headers, but a clean rebuild is cheap and avoids mixed object ABIs after
// heic_internal.h layout edits.
//
// The corpus directory (fuzz/corpus) IS the checkpoint: stop by killing the
// process, resume by running again — libFuzzer reloads the corpus and keeps
// going. Crashes land in fuzz/crashes (tracked in git as regression seeds);
// each is a file-shaped input reproducible with
//   out/fuzz/heic_fuzz.exe <crashfile>
//
// No extra install on Windows: libFuzzer + ASan ship with the VS-bundled clang
// (or LLVM clang on PATH). On macOS use Homebrew llvm (see buildFuzz).
import {
  existsSync,
  mkdirSync,
  readdirSync,
  copyFileSync,
  renameSync,
  rmSync,
  statSync,
} from "node:fs";
import { resolve, join, basename } from "node:path";
import {
  buildFuzz,
  buildDav1d,
  buildZlib,
  buildBrotli,
  dav1dPaths,
  zlibPaths,
  brotliPaths,
  FUZZ_EXE,
} from "./build";
import { getDeps } from "./get-deps";
import { corpusFiles } from "./corpus";

const ROOT = resolve(import.meta.dir, "..");
const FUZZ = join(ROOT, "fuzz");
const CORPUS = join(FUZZ, "corpus");
const CRASHES = join(FUZZ, "crashes");

function usage(): never {
  console.error(
    `usage: bun cmd/fuzz.ts [options]
  (no args)      build, seed corpus if empty, fuzz until killed (resumes)
  -jobs N        run N parallel workers sharing the corpus
  -max-len N     max input size in bytes (default 4000000)
  -repro FILE    replay a single crash artifact and exit
  -check-crashes replay every fuzz/crashes artifact under ASan (CI regression)
  -minimize      shrink the corpus to a minimal covering set
  -no-deps       skip auto-building dav1d/zlib/brotli (HEVC-only fuzz)
  -h, --help`,
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let jobs = 1;
let maxLen = 4000000;
let repro = "";
let checkCrashes = false;
let minimize = false;
let noDeps = false;

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === "-jobs") jobs = intArg(args[++i], "-jobs");
  else if (a === "-max-len") maxLen = intArg(args[++i], "-max-len");
  else if (a === "-repro") repro = args[++i] ?? usage();
  else if (a === "-check-crashes") checkCrashes = true;
  else if (a === "-minimize") minimize = true;
  else if (a === "-no-deps") noDeps = true;
  else if (a === "-h" || a === "--help") usage();
  else usage();
}

function intArg(v: string | undefined, name: string): number {
  const n = Number.parseInt(v ?? "", 10);
  if (!Number.isFinite(n) || n < 1) {
    console.error(`${name} requires a positive integer`);
    process.exit(2);
  }
  return n;
}

// Artifact kinds libFuzzer writes on a finding (all reproducible by replay).
const isArtifact = (name: string) =>
  /^(crash|timeout|oom|leak|slow-unit)-/.test(name);

function listArtifacts(): string[] {
  if (!existsSync(CRASHES)) return [];
  return readdirSync(CRASHES).filter(isArtifact);
}

async function run(argv: string[]): Promise<number> {
  const proc = Bun.spawn([FUZZ_EXE, ...argv], {
    cwd: FUZZ,
    stdout: "inherit",
    stderr: "inherit",
  });
  return await proc.exited;
}

// Optional codecs expand surface (AVIF via dav1d, unci deflate/brotli).
// Skip with -no-deps for a pure-C HEVC-only harness.
// Missing build tools (meson/cmake/ninja) exit with winget install hints —
// we do not dig through install dirs for them.
if (!noDeps) {
  await getDeps();
  if (!dav1dPaths()) {
    console.log("building dav1d for AVIF fuzz coverage...");
    await buildDav1d();
  }
  if (!zlibPaths()) await buildZlib();
  if (!brotliPaths()) await buildBrotli();
}

const exe = await buildFuzz(true); // always clean-build (see header comment)

mkdirSync(CORPUS, { recursive: true });
mkdirSync(CRASHES, { recursive: true });

// -repro: replay one artifact with a full stack trace, then exit.
if (repro) {
  const path = resolve(repro);
  if (!existsSync(path)) {
    console.error(`no such file: ${path}`);
    process.exit(1);
  }
  console.log(`replaying ${path}`);
  process.exit(await run([path]));
}

// -check-crashes: every tracked artifact must complete without ASan/libFuzzer
// crash (exit 0). Used by CI as a fixed regression suite.
if (checkCrashes) {
  const arts = listArtifacts();
  if (arts.length === 0) {
    console.log("no crash/slow artifacts in fuzz/crashes — nothing to check");
    process.exit(0);
  }
  let fail = 0;
  console.log(`checking ${arts.length} fuzz/crashes artifact(s) under ASan...`);
  for (const name of arts) {
    const path = join(CRASHES, name);
    const rc = await run([path]);
    if (rc !== 0) {
      fail++;
      console.error(`[fail] ${name} exit=${rc}`);
    } else {
      console.log(`[ok] ${name}`);
    }
  }
  if (fail) {
    console.error(`${fail}/${arts.length} artifact(s) still crash`);
    process.exit(1);
  }
  console.log(`check-crashes: ${arts.length} ok`);
  process.exit(0);
}

// -minimize: merge the corpus into a fresh minimal covering set, then swap.
if (minimize) {
  const tmp = join(FUZZ, "corpus.min");
  rmSync(tmp, { recursive: true, force: true });
  mkdirSync(tmp, { recursive: true });
  console.log("minimizing corpus (libFuzzer -merge=1)...");
  const rc = await run(["-merge=1", "corpus.min", "corpus"]);
  if (rc !== 0) {
    console.error(`merge failed (${rc}); leaving corpus untouched`);
    rmSync(tmp, { recursive: true, force: true });
    process.exit(rc);
  }
  const before = readdirSync(CORPUS).length;
  rmSync(CORPUS, { recursive: true, force: true });
  renameSync(tmp, CORPUS);
  const after = readdirSync(CORPUS).length;
  console.log(`corpus minimized: ${before} -> ${after} inputs`);
  process.exit(0);
}

// First run: seed the (empty) corpus with real HEIC/HEIF/AVIF files so
// mutation starts from valid containers, not random bytes. Files over max_len
// are skipped — libFuzzer would ignore them anyway.
if (readdirSync(CORPUS).length === 0) {
  await getDeps();
  let n = 0;
  for (const f of corpusFiles()) {
    if (statSync(f).size > maxLen) continue;
    // Unique names: corpus has basename collisions across roots.
    const base = basename(f);
    let dest = join(CORPUS, base);
    if (existsSync(dest)) {
      dest = join(CORPUS, `${n}_${base}`);
    }
    copyFileSync(f, dest);
    n++;
  }
  console.log(`seeded corpus with ${n} file(s) from the deps/ corpus`);
}

const before = new Set(listArtifacts());

// Swallow SIGINT in the parent so that when you Ctrl-C, the child (which shares
// the console and receives the signal too) exits first and we still print the
// crash summary below.
process.on("SIGINT", () => {});

const fuzzArgs = [
  "corpus",
  "-artifact_prefix=crashes/",
  `-max_len=${maxLen}`,
  "-rss_limit_mb=4096",
  "-timeout=25",
  // Large seeds (Nokia grids, etc.) can cost several seconds under ASan.
  "-report_slow_units=20",
  "-print_final_stats=1",
  ...(jobs > 1 ? [`-jobs=${jobs}`, `-workers=${jobs}`] : []),
];

console.log(
  `fuzzing (${jobs} ${jobs === 1 ? "process" : "workers"}); ` +
    `corpus=${CORPUS}\n  Ctrl-C to stop; rerun to resume.`,
);
const rc = await run(fuzzArgs);

const fresh = listArtifacts().filter((f) => !before.has(f));
if (fresh.length > 0) {
  console.log(`\n${fresh.length} new crash artifact(s) in fuzz/crashes:`);
  for (const f of fresh) {
    console.log(`  ${f}`);
    console.log(`    reproduce: ${exe} ${join(CRASHES, f)}`);
    console.log(`    or:        bun cmd/fuzz.ts -repro ${join("fuzz", "crashes", f)}`);
  }
  process.exit(1);
}
process.exit(rc);
