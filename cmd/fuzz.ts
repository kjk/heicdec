// fuzz.ts -- coverage-guided fuzzing of the HEIC/HEIF/AVIF decoder (libFuzzer).
//
//   bun cmd/fuzz.ts                    fuzz the whole-file target until killed
//   bun cmd/fuzz.ts -target hevc       fuzz another target (see -h)
//   bun cmd/fuzz.ts -jobs 8            8 parallel workers sharing the corpus
//   bun cmd/fuzz.ts -repro F           replay a single crash artifact and exit
//   bun cmd/fuzz.ts -minimize          shrink the corpus to a minimal covering set
//   bun cmd/fuzz.ts -coverage          which code the corpus never reaches
//
// Targets (test/fuzz_*.c), each with its own corpus and crash directory:
//   heic       whole file: open, decode, metadata        fuzz/corpus
//   container  ISOBMFF/HEIF parsing only, very fast      fuzz/corpus-container
//   hevc       raw Annex B HEVC, no container            fuzz/corpus-hevc
//   diff       pixels compared with libheif (*nix)       fuzz/corpus-diff
//
// A corpus directory IS the checkpoint: stop by killing the process, resume
// by running again — libFuzzer reloads the corpus and keeps going. Crashes
// land in fuzz/crashes[-<target>] (tracked in git as regression seeds); each
// is reproducible with `bun cmd/fuzz.ts -target T -repro <file>`.
//
// Sanitizers: ASan everywhere; UBSan too on Linux/macOS (findings are fatal);
// -msan swaps ASan for MemorySanitizer (Linux, pure-C build without
// dav1d/zlib/brotli). The whole-file targets also get an ISOBMFF-aware
// mutator (test/fuzz_mutator.c) and a dictionary (fuzz/*.dict).
//
// No extra install on Windows: libFuzzer + ASan ship with the VS-bundled clang
// (or LLVM clang on PATH). On macOS use Homebrew llvm (see buildFuzz).
import { existsSync, mkdirSync, readdirSync, renameSync, rmSync } from "node:fs";
import { resolve, join } from "node:path";
import {
  buildFuzz,
  buildDav1d,
  buildZlib,
  buildBrotli,
  dav1dPaths,
  zlibPaths,
  brotliPaths,
  findLlvmTool,
  FUZZ_TARGETS,
  isFuzzTarget,
  isWindows,
  type FuzzBuildOpts,
  type FuzzTarget,
} from "./build";
import { getDeps } from "./get-deps";
import {
  copyNew,
  corpusDir,
  crashesDir,
  dictPath,
  FUZZ,
  FUZZ_DIRS,
  listArtifacts,
  ROOT,
  seedCorpus,
} from "./fuzz-common";

function usage(): never {
  const targets = (Object.keys(FUZZ_TARGETS) as FuzzTarget[])
    .map((t) => `                   ${t.padEnd(10)} ${FUZZ_TARGETS[t].desc}`)
    .join("\n");
  console.error(
    `usage: bun cmd/fuzz.ts [options]
  (no args)      build, seed corpus if empty, fuzz until killed (resumes)
  -target T      what to fuzz (default heic):
${targets}
  -jobs N        run N parallel workers sharing the corpus
  -time SEC      stop after SEC seconds
  -max-len N     max input size in bytes (default: 262144 for heic and diff,
                 65536 for container and hevc)
  -reseed        add the seeds again (after the deps corpus grew)
  -repro FILE    replay a single crash artifact and exit
  -check-crashes replay the tracked artifacts (CI regression); without -target
                 every target that has any
  -minimize      shrink the corpus to a minimal covering set
  -coverage      run the corpus through a coverage build and report the code
                 it never reaches (HTML under out/fuzz/cov/)
  -msan          MemorySanitizer instead of ASan (Linux; pure-C build, no
                 dav1d/zlib/brotli)
  -ubsan         add UBSan (default on Linux/macOS)   -no-ubsan  leave it out
  -no-mutator    plain libFuzzer mutations only (no ISOBMFF-aware mutator)
  -no-deps       do not build dav1d/zlib/brotli when missing (ones already
                 built under out/ are still linked)
  -build-only    build the target(s) and exit (with no -target: all of them)
  -clean         rebuild from scratch
  -h, --help`,
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let targetArg = "";
let jobs = 1;
let maxLenArg = 0;
let timeSec = 0;
let repro = "";
let checkCrashes = false;
let minimize = false;
let coverage = false;
let msan = false;
let ubsan = !isWindows;
let noMutator = false;
let noDeps = false;
let reseed = false;
let buildOnly = false;
let clean = false;

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === "-target") targetArg = args[++i] ?? usage();
  else if (a === "-jobs") jobs = intArg(args[++i], "-jobs");
  else if (a === "-max-len") maxLenArg = intArg(args[++i], "-max-len");
  else if (a === "-time") timeSec = intArg(args[++i], "-time");
  else if (a === "-repro") repro = args[++i] ?? usage();
  else if (a === "-check-crashes") checkCrashes = true;
  else if (a === "-minimize") minimize = true;
  else if (a === "-coverage") coverage = true;
  else if (a === "-msan") msan = true;
  else if (a === "-ubsan") ubsan = true;
  else if (a === "-no-ubsan") ubsan = false;
  else if (a === "-no-mutator") noMutator = true;
  else if (a === "-no-deps") noDeps = true;
  else if (a === "-reseed") reseed = true;
  else if (a === "-build-only") buildOnly = true;
  else if (a === "-clean") clean = true;
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

if (targetArg && !isFuzzTarget(targetArg)) {
  console.error(`unknown target: ${targetArg}`);
  usage();
}
const target: FuzzTarget = targetArg ? (targetArg as FuzzTarget) : "heic";
if (msan) noDeps = true;
if (ubsan && isWindows) {
  console.error("-ubsan is not supported on Windows");
  process.exit(2);
}
const maxLen = maxLenArg || FUZZ_DIRS[target].maxLen;

function buildOpts(t: FuzzTarget, extra: FuzzBuildOpts = {}): FuzzBuildOpts {
  return {
    target: t,
    sanitizer: msan ? "msan" : "asan",
    ubsan,
    /* -no-deps only skips building the codecs; ones already built are still
       linked. MSan is the build that must leave them out. */
    noDeps: msan,
    clean,
    ...extra,
  };
}

/** Sanitizer runtime settings: symbolized reports, and UBSan findings fatal. */
function fuzzEnv(): Record<string, string | undefined> {
  const env: Record<string, string | undefined> = { ...process.env };
  const symbolizer = findLlvmTool("llvm-symbolizer");
  if (symbolizer) {
    env.ASAN_SYMBOLIZER_PATH ??= symbolizer;
    env.MSAN_SYMBOLIZER_PATH ??= symbolizer;
    env.UBSAN_SYMBOLIZER_PATH ??= symbolizer;
  }
  env.UBSAN_OPTIONS ??= "print_stacktrace=1:halt_on_error=1";
  if (noMutator) env.HEIC_FUZZ_NO_MUTATOR = "1";
  return env;
}

async function run(
  exe: string,
  argv: string[],
  env: Record<string, string | undefined> = fuzzEnv(),
): Promise<number> {
  const proc = Bun.spawn([exe, ...argv], {
    cwd: FUZZ,
    stdout: "inherit",
    stderr: "inherit",
    env,
  });
  return await proc.exited;
}

async function runCapture(argv: string[]): Promise<{ rc: number; out: string }> {
  const proc = Bun.spawn(argv, { cwd: ROOT, stdout: "pipe", stderr: "inherit" });
  const out = await new Response(proc.stdout).text();
  return { rc: await proc.exited, out };
}

// Optional codecs expand surface (AVIF via dav1d, unci deflate/brotli).
// Skip with -no-deps for a pure-C HEVC-only harness.
// Missing build tools (meson/cmake/ninja) exit with install hints —
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

mkdirSync(FUZZ, { recursive: true });

// -build-only: keep the targets compiling (CI). Without -target, every target
// this platform can build.
if (buildOnly) {
  const all = (Object.keys(FUZZ_TARGETS) as FuzzTarget[]).filter(
    (t) => !FUZZ_TARGETS[t].libheif,
  );
  for (const t of targetArg ? [target] : all) await buildFuzz(buildOpts(t));
  process.exit(0);
}

// -check-crashes: every tracked artifact must complete without a sanitizer or
// libFuzzer crash (exit 0). Used by CI as a fixed regression suite.
if (checkCrashes) {
  const all = (Object.keys(FUZZ_TARGETS) as FuzzTarget[]).filter(
    (t) => !(FUZZ_TARGETS[t].libheif && isWindows),
  );
  let total = 0;
  let fail = 0;
  for (const t of targetArg ? [target] : all) {
    const arts = listArtifacts(t);
    if (arts.length === 0) continue;
    const exe = await buildFuzz(buildOpts(t));
    console.log(`checking ${arts.length} ${FUZZ_DIRS[t].crashes} artifact(s) with target ${t}...`);
    for (const name of arts) {
      const rc = await run(exe, [join(crashesDir(t), name)]);
      total++;
      if (rc !== 0) {
        fail++;
        console.error(`[fail] ${t}: ${name} exit=${rc}`);
      } else {
        console.log(`[ok] ${t}: ${name}`);
      }
    }
  }
  if (total === 0) {
    console.log("no crash/slow artifacts tracked — nothing to check");
    process.exit(0);
  }
  if (fail) {
    console.error(`${fail}/${total} artifact(s) still crash`);
    process.exit(1);
  }
  console.log(`check-crashes: ${total} ok`);
  process.exit(0);
}

const CORPUS = corpusDir(target);
const CRASHES = crashesDir(target);
mkdirSync(CORPUS, { recursive: true });
mkdirSync(CRASHES, { recursive: true });

// First run (or -reseed): seed the corpus with real files so mutation starts
// from valid input, not random bytes. Files over max_len are cut to it
// rather than skipped: the head of a big file keeps its whole box structure.
async function seedIfNeeded(): Promise<void> {
  if (!reseed && readdirSync(CORPUS).length > 0) return;
  await getDeps();
  const n = seedCorpus(target, maxLen);
  console.log(`seeded ${FUZZ_DIRS[target].corpus} with ${n} file(s) (max ${maxLen} bytes each)`);
  /* The differential target also profits from what the whole-file fuzzer
     already found. */
  if (target === "diff") {
    const m = copyNew(corpusDir("heic"), CORPUS);
    if (m) console.log(`  + ${m} input(s) from fuzz/corpus`);
  }
}

// -coverage: what does the corpus not reach? Build with source-based coverage
// (no sanitizer), run every input once, and report per-file coverage and the
// functions never entered. Blind spots are where new seeds pay off.
if (coverage) {
  const profdata = findLlvmTool("llvm-profdata");
  const cov = findLlvmTool("llvm-cov");
  if (!profdata || !cov) {
    console.error(
      "llvm-profdata / llvm-cov not found (they ship with LLVM; on " +
        "Debian/Ubuntu: apt install llvm)",
    );
    process.exit(2);
  }
  await seedIfNeeded();
  const exe = await buildFuzz(buildOpts(target, { coverage: true, ubsan: false }));
  const dir = join(ROOT, "out", "fuzz", "cov");
  const raw = join(dir, `${target}.profraw`);
  const merged = join(dir, `${target}.profdata`);
  rmSync(raw, { force: true });
  console.log(`running ${readdirSync(CORPUS).length} corpus input(s) for coverage...`);
  const rc = await run(exe, ["-runs=0", "-rss_limit_mb=4096", "-timeout=60", CORPUS], {
    ...process.env,
    LLVM_PROFILE_FILE: raw,
  });
  if (rc !== 0 || !existsSync(raw)) {
    console.error(`coverage run failed (exit ${rc})`);
    process.exit(1);
  }
  await runCapture([profdata, "merge", "-sparse", raw, "-o", merged]);
  const srcDir = join(ROOT, "src");
  const report = await runCapture([
    cov, "report", exe, `-instr-profile=${merged}`, srcDir,
  ]);
  console.log(report.out);
  const html = join(dir, `${target}-html`);
  await runCapture([
    cov, "show", exe, `-instr-profile=${merged}`, "-format=html",
    `-output-dir=${html}`, srcDir,
  ]);
  const exported = await runCapture([
    cov, "export", exe, `-instr-profile=${merged}`, srcDir,
  ]);
  try {
    type Fn = { name: string; count: number; filenames: string[] };
    const srcPrefix = srcDir.replaceAll("\\", "/") + "/";
    /* Functions of the .c files only: an inline helper in a header shows up
       once per file that includes it. */
    const fns = (JSON.parse(exported.out).data[0].functions as Fn[]).filter((f) => {
      const file = f.filenames[0]?.replaceAll("\\", "/") ?? "";
      return f.count === 0 && file.startsWith(srcPrefix) && file.endsWith(".c");
    });
    const byFile = new Map<string, Set<string>>();
    for (const f of fns) {
      const file = f.filenames[0]!.replaceAll("\\", "/").slice(srcPrefix.length);
      const name = f.name.includes(":") ? f.name.slice(f.name.indexOf(":") + 1) : f.name;
      if (!byFile.has(file)) byFile.set(file, new Set());
      byFile.get(file)!.add(name);
    }
    const total = [...byFile.values()].reduce((n, set) => n + set.size, 0);
    console.log(`${total} function(s) in src/*.c never entered by this corpus:`);
    for (const [file, names] of [...byFile].sort()) {
      console.log(`  ${file} (${names.size}): ${[...names].sort().join(", ")}`);
    }
  } catch {
    console.error("could not parse llvm-cov export output");
  }
  console.log(`\nline-by-line report: ${join(html, "index.html")}`);
  process.exit(0);
}

const exe = await buildFuzz(buildOpts(target));

// -repro: replay one artifact with a full stack trace, then exit.
if (repro) {
  const path = resolve(repro);
  if (!existsSync(path)) {
    console.error(`no such file: ${path}`);
    process.exit(1);
  }
  console.log(`replaying ${path}`);
  process.exit(await run(exe, [path]));
}

// -minimize: merge the corpus into a fresh minimal covering set, then swap.
if (minimize) {
  const tmp = `${CORPUS}.min`;
  rmSync(tmp, { recursive: true, force: true });
  mkdirSync(tmp, { recursive: true });
  console.log("minimizing corpus (libFuzzer -merge=1)...");
  const rc = await run(exe, ["-merge=1", `-max_len=${maxLen}`, tmp, CORPUS]);
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

await seedIfNeeded();

const before = new Set(listArtifacts(target));

// Swallow SIGINT in the parent so that when you Ctrl-C, the child (which shares
// the console and receives the signal too) exits first and we still print the
// crash summary below.
process.on("SIGINT", () => {});

const dict = dictPath(target);
const fuzzArgs = [
  CORPUS,
  `-artifact_prefix=${CRASHES}/`,
  `-max_len=${maxLen}`,
  ...(existsSync(dict) ? [`-dict=${dict}`] : []),
  "-rss_limit_mb=4096",
  "-timeout=25",
  // A slow input is worth knowing about, but should not flood the log.
  "-report_slow_units=20",
  "-print_final_stats=1",
  ...(timeSec ? [`-max_total_time=${timeSec}`] : []),
  /* The differential target reports leads, some of them false (see
     test/fuzz_diff.c). Fork mode keeps fuzzing past each one and collects
     them all, instead of stopping at the first. */
  ...(target === "diff"
    ? [`-fork=${jobs}`, "-ignore_crashes=1"]
    : jobs > 1
      ? [`-jobs=${jobs}`, `-workers=${jobs}`]
      : []),
];

console.log(
  `fuzzing ${target} (${jobs} ${jobs === 1 ? "process" : "workers"}, ` +
    `${msan ? "msan" : "asan"}${ubsan ? "+ubsan" : ""}); corpus=${CORPUS}\n` +
    `  Ctrl-C to stop; rerun to resume.`,
);
const rc = await run(exe, fuzzArgs);

const fresh = listArtifacts(target).filter((f) => !before.has(f));
if (fresh.length > 0) {
  console.log(`\n${fresh.length} new crash artifact(s) in ${CRASHES}:`);
  for (const f of fresh) {
    console.log(`  ${f}`);
    console.log(
      `    reproduce: bun cmd/fuzz.ts -target ${target} -repro ${join("fuzz", FUZZ_DIRS[target].crashes, f)}`,
    );
  }
  process.exit(1);
}
process.exit(rc);
