// fuzz-afl.ts -- coverage-guided fuzzing with AFL++ (macOS / *nix).
//
//   bun cmd/fuzz-afl.ts             build, seed corpus if empty, fuzz until killed
//   bun cmd/fuzz-afl.ts -jobs 4     1 main + 3 secondary instances (same -o dir)
//   bun cmd/fuzz-afl.ts -repro F    replay a single crash artifact and exit
//   bun cmd/fuzz-afl.ts -import     copy AFL queue/crashes into shared corpus/crashes
//   bun cmd/fuzz-afl.ts -cmin       minimize shared corpus with afl-cmin
//   bun cmd/fuzz-afl.ts -V 60       stop after 60 seconds (smoke / CI)
//
// Shares the seed corpus with libFuzzer:
//   fuzz/corpus/     seeds for both bun cmd/fuzz.ts and this script
//   fuzz/afl-out/    AFL++ state (queue, hangs, crashes, fuzzer_stats) — gitignored
//   fuzz/crashes/    tracked regression seeds (libFuzzer + imported AFL crashes)
//
// Same harness as libFuzzer: test/fuzz_target.c (LLVMFuzzerTestOneInput).
// Built with afl-clang-fast -fsanitize=fuzzer → AFL++ libAFLDriver + persistent
// mode. Resume is automatic when fuzz/afl-out already has a campaign.
//
// macOS once-per-machine setup (raises SysV shm limits; needs root):
//   sudo afl-system-config
// Without that, this script still tries by setting AFL_MAP_SIZE from the binary
// so the coverage map fits under the default 4 MB kern.sysv.shmmax.
import {
  existsSync,
  mkdirSync,
  readdirSync,
  copyFileSync,
  statSync,
  readFileSync,
  rmSync,
  renameSync,
} from "node:fs";
import { resolve, join, basename } from "node:path";
import { createHash } from "node:crypto";
import {
  buildAflFuzz,
  buildDav1d,
  buildZlib,
  buildBrotli,
  dav1dPaths,
  zlibPaths,
  brotliPaths,
  aflTool,
  isWindows,
  isMac,
} from "./build";
import { getDeps } from "./get-deps";
import { corpusFiles } from "./corpus";

const FUZZ = resolve(import.meta.dir, "..", "fuzz");
const CORPUS = join(FUZZ, "corpus");
const CRASHES = join(FUZZ, "crashes");
const AFL_OUT = join(FUZZ, "afl-out");

function usage(): never {
  console.error(
    `usage: bun cmd/fuzz-afl.ts [options]
  (no args)      build, seed corpus if empty, afl-fuzz until killed (resumes)
  -jobs N        parallel AFL instances sharing -o (1 main + N-1 secondaries)
  -max-len N     max input size in bytes (default 1000000; AFL MAX_FILE)
  -repro FILE    replay a single crash artifact with the AFL harness and exit
  -import        merge AFL queue → fuzz/corpus and AFL crashes → fuzz/crashes
  -cmin          minimize fuzz/corpus with afl-cmin (writes corpus, keeps backup)
  -no-asan       build without AddressSanitizer (faster)
  -no-deps       skip auto-building dav1d/zlib/brotli (HEVC-only fuzz)
  -V SEC         stop after SEC seconds of fuzz time (afl-fuzz -V)
  -clean         force rebuild of the AFL harness
  -h, --help

Corpus is shared with bun cmd/fuzz.ts (fuzz/corpus/). AFL state lives in
fuzz/afl-out/. On macOS run once: sudo afl-system-config
`,
  );
  process.exit(2);
}

const args = process.argv.slice(2);
let jobs = 1;
let maxLen = 1_000_000; // AFL++ default MAX_FILE; raise with care (shm budget)
let repro = "";
let doImport = false;
let cmin = false;
let noAsan = false;
let noDeps = false;
let timeLimitSec = 0;
let clean = false;

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === "-jobs") jobs = intArg(args[++i], "-jobs");
  else if (a === "-max-len") maxLen = intArg(args[++i], "-max-len");
  else if (a === "-repro") repro = args[++i] ?? usage();
  else if (a === "-import") doImport = true;
  else if (a === "-cmin") cmin = true;
  else if (a === "-no-asan") noAsan = true;
  else if (a === "-no-deps") noDeps = true;
  else if (a === "-V") timeLimitSec = intArg(args[++i], "-V");
  else if (a === "-clean") clean = true;
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

if (isWindows) {
  console.error("AFL++ fuzzing is for macOS/*nix. On Windows use: bun cmd/fuzz.ts");
  process.exit(2);
}

function requireTool(name: string): string {
  const p = aflTool(name);
  if (!p) {
    console.error(`${name} not found. On macOS: brew install afl++`);
    process.exit(2);
  }
  return p;
}

/** Seed fuzz/corpus from deps/ when empty (shared with libFuzzer). */
function seedCorpusIfEmpty(limit: number): void {
  mkdirSync(CORPUS, { recursive: true });
  if (readdirSync(CORPUS).length > 0) return;
  let n = 0;
  for (const f of corpusFiles()) {
    if (statSync(f).size > limit) continue;
    const base = basename(f);
    let dest = join(CORPUS, base);
    if (existsSync(dest)) dest = join(CORPUS, `${n}_${base}`);
    copyFileSync(f, dest);
    n++;
  }
  console.log(`seeded corpus with ${n} file(s) from the deps/ corpus`);
}

/** List files under dir (non-recursive) that look like fuzzer inputs. */
function listFiles(dir: string): string[] {
  if (!existsSync(dir)) return [];
  return readdirSync(dir).filter((n) => {
    if (n.startsWith(".")) return false;
    try {
      return statSync(join(dir, n)).isFile();
    } catch {
      return false;
    }
  });
}

/** AFL campaign instance dirs under -o (default, main, secondary-*, …). */
function aflInstanceDirs(outDir: string): string[] {
  if (!existsSync(outDir)) return [];
  return readdirSync(outDir)
    .map((n) => join(outDir, n))
    .filter((p) => {
      try {
        return statSync(p).isDirectory() && existsSync(join(p, "queue"));
      } catch {
        return false;
      }
    });
}

function campaignExists(outDir: string): boolean {
  return aflInstanceDirs(outDir).length > 0;
}

/**
 * Import AFL-discovered queue entries into the shared libFuzzer corpus, and
 * AFL crashes into fuzz/crashes (content-hashed names with an afl- prefix).
 *
 * Queue files that still carry `,orig:` are the initial seed copies from
 * fuzz/corpus — skip those so we only share new coverage back to libFuzzer.
 */
function importAflFindings(): { queue: number; crashes: number } {
  mkdirSync(CORPUS, { recursive: true });
  mkdirSync(CRASHES, { recursive: true });
  let queue = 0;
  let crashes = 0;
  for (const inst of aflInstanceDirs(AFL_OUT)) {
    const qdir = join(inst, "queue");
    for (const name of listFiles(qdir)) {
      if (name === "README.txt") continue;
      // Seed clones look like id:000000,time:0,execs:0,orig:<name>
      if (name.includes(",orig:")) continue;
      const src = join(qdir, name);
      const data = readFileSync(src);
      const hash = createHash("sha1").update(data).digest("hex").slice(0, 16);
      const dest = join(CORPUS, `afl-${hash}`);
      if (existsSync(dest)) continue;
      copyFileSync(src, dest);
      queue++;
    }
    const cdir = join(inst, "crashes");
    for (const name of listFiles(cdir)) {
      if (name === "README.txt") continue;
      const src = join(cdir, name);
      const data = readFileSync(src);
      const hash = createHash("sha1").update(data).digest("hex");
      const dest = join(CRASHES, `afl-${hash}`);
      if (existsSync(dest)) continue;
      copyFileSync(src, dest);
      crashes++;
    }
  }
  return { queue, crashes };
}

/** Coverage-map size the instrumented binary expects (AFL_DUMP_MAP_SIZE). */
async function dumpMapSize(exe: string): Promise<number | null> {
  const proc = Bun.spawn([exe], {
    cwd: FUZZ,
    env: { ...process.env, AFL_DUMP_MAP_SIZE: "1" },
    stdout: "pipe",
    stderr: "pipe",
  });
  const [stdout, stderr] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);
  const text = `${stdout}\n${stderr}`;
  const m = text.match(/^\s*(\d+)\s*$/m) || text.trim().match(/^(\d+)$/);
  if (!m) return null;
  const n = Number.parseInt(m[1], 10);
  return Number.isFinite(n) && n > 0 ? n : null;
}

function macShmMax(): number | null {
  if (!isMac) return null;
  try {
    const r = Bun.spawnSync(["sysctl", "-n", "kern.sysv.shmmax"]);
    const n = Number.parseInt(r.stdout.toString().trim(), 10);
    return Number.isFinite(n) ? n : null;
  } catch {
    return null;
  }
}

function baseAflEnv(mapSize: number | null): Record<string, string> {
  const env: Record<string, string> = {
    ...process.env,
    // macOS: ReportCrash / CPU scaling noise; safe no-ops elsewhere.
    AFL_SKIP_CPUFREQ: process.env.AFL_SKIP_CPUFREQ ?? "1",
    AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES:
      process.env.AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES ?? "1",
    // Import shared-corpus discoveries from other instances / libFuzzer adds.
    AFL_AUTORESUME: process.env.AFL_AUTORESUME ?? "1",
    // Large HEIC seeds; skip slow calibration when resuming big queues.
    AFL_FAST_CAL: process.env.AFL_FAST_CAL ?? "1",
    ASAN_OPTIONS:
      process.env.ASAN_OPTIONS ??
      "abort_on_error=1:detect_leaks=0:symbolize=0:allocator_may_return_null=1",
  };
  // Default AFL shmem is 8 MB; stock macOS kern.sysv.shmmax is 4 MB.
  // Pin map size from the binary so shmget stays under the limit.
  if (mapSize && !process.env.AFL_MAP_SIZE) {
    // Round up to next power-of-two-ish headroom (AFL accepts exact sizes).
    env.AFL_MAP_SIZE = String(Math.max(mapSize + 1, 65536));
  }
  return env;
}

async function runCapture(
  argv: string[],
  env: Record<string, string>,
): Promise<number> {
  const proc = Bun.spawn(argv, {
    cwd: FUZZ,
    env,
    stdout: "inherit",
    stderr: "inherit",
  });
  return await proc.exited;
}

// ---- deps + build ----------------------------------------------------------

if (!noDeps) {
  await getDeps();
  if (!dav1dPaths()) {
    console.log("building dav1d for AVIF fuzz coverage...");
    await buildDav1d();
  }
  if (!zlibPaths()) await buildZlib();
  if (!brotliPaths()) await buildBrotli();
}

const exe = await buildAflFuzz({ clean, asan: !noAsan });
mkdirSync(CORPUS, { recursive: true });
mkdirSync(CRASHES, { recursive: true });
mkdirSync(AFL_OUT, { recursive: true });

// -repro: run the harness on one file (AFL driver accepts path args).
if (repro) {
  const path = resolve(repro);
  if (!existsSync(path)) {
    console.error(`no such file: ${path}`);
    process.exit(1);
  }
  console.log(`replaying ${path}`);
  process.exit(await runCapture([exe, path], baseAflEnv(null)));
}

// -import only: merge AFL findings into shared dirs, then exit.
if (doImport && !cmin) {
  const r = importAflFindings();
  console.log(
    `imported ${r.queue} queue input(s) → ${CORPUS}, ` +
      `${r.crashes} crash(es) → ${CRASHES}`,
  );
  process.exit(0);
}

// Ensure seeds exist before cmin / fuzz.
if (readdirSync(CORPUS).length === 0) {
  await getDeps();
  seedCorpusIfEmpty(maxLen);
}
if (readdirSync(CORPUS).length === 0) {
  console.error("corpus is empty; fetch deps (bun cmd/get-deps.ts) and retry");
  process.exit(1);
}

// -cmin: minimize shared corpus in place (backup at fuzz/corpus. precmin).
if (cmin) {
  const aflCmin = requireTool("afl-cmin");
  const mapSize = await dumpMapSize(exe);
  const env = baseAflEnv(mapSize);
  const tmp = join(FUZZ, "corpus.cmin");
  rmSync(tmp, { recursive: true, force: true });
  mkdirSync(tmp, { recursive: true });
  console.log("minimizing corpus (afl-cmin)...");
  const rc = await runCapture(
    [aflCmin, "-i", "corpus", "-o", "corpus.cmin", "-t", "25000", "--", exe],
    env,
  );
  if (rc !== 0) {
    console.error(`afl-cmin failed (${rc}); leaving corpus untouched`);
    rmSync(tmp, { recursive: true, force: true });
    process.exit(rc);
  }
  const before = readdirSync(CORPUS).length;
  const bak = join(FUZZ, "corpus.precmin");
  rmSync(bak, { recursive: true, force: true });
  renameSync(CORPUS, bak);
  renameSync(tmp, CORPUS);
  const after = readdirSync(CORPUS).length;
  console.log(
    `corpus minimized: ${before} -> ${after} inputs (backup: fuzz/corpus.precmin)`,
  );
  if (doImport) {
    /* no-op: cmin already rewrote corpus */
  }
  process.exit(0);
}

// ---- map size / macOS shm guard --------------------------------------------

const mapSize = await dumpMapSize(exe);
if (mapSize) {
  console.log(`AFL coverage map size: ${mapSize} bytes`);
}
const shmMax = macShmMax();
const needShm = (mapSize ?? 65536) + 1_000_000 + 4096; // map + fuzz input shm + slack
if (shmMax !== null && needShm > shmMax) {
  console.error(
    `\nmacOS SysV shared-memory limit is too small for this target:\n` +
      `  kern.sysv.shmmax=${shmMax}  need≈${needShm}\n` +
      `Raise limits once (requires root):\n` +
      `  sudo afl-system-config\n` +
      `Then re-run. (Also disables ReportCrash delays that mis-label crashes.)\n`,
  );
  process.exit(2);
}
if (isMac && shmMax !== null && shmMax < 8 * 1024 * 1024) {
  console.log(
    `note: kern.sysv.shmmax=${shmMax} (stock macOS is 4 MiB). ` +
      `Using AFL_MAP_SIZE workaround; for multi-job fuzzing prefer:\n` +
      `  sudo afl-system-config`,
  );
}

// Feed any new shared-corpus seeds into an existing AFL campaign (libFuzzer
// or a previous import may have added them).
if (campaignExists(AFL_OUT)) {
  const addseeds = aflTool("afl-addseeds");
  if (addseeds) {
    console.log("adding new shared-corpus seeds into existing AFL campaign...");
    await runCapture([addseeds, "-o", AFL_OUT, CORPUS], baseAflEnv(mapSize));
  }
}

const aflFuzz = requireTool("afl-fuzz");
const env = baseAflEnv(mapSize);

const beforeCrashes = new Set(listFiles(CRASHES));

// Parent swallows SIGINT so children finish and we can import findings.
process.on("SIGINT", () => {});

console.log(
  `AFL++ fuzzing (${jobs} ${jobs === 1 ? "instance" : "instances"}); ` +
    `seeds=${CORPUS}\n  state=${AFL_OUT}\n  Ctrl-C to stop; rerun to resume; ` +
    `bun cmd/fuzz-afl.ts -import merges queue → shared corpus.`,
);

const children: ReturnType<typeof Bun.spawn>[] = [];

/** Per-instance -i: resume with '-' only when that instance already has a queue. */
function spawnAfl(
  role: "-M" | "-S",
  name: string,
  quiet: boolean,
): ReturnType<typeof Bun.spawn> {
  const resume = existsSync(join(AFL_OUT, name, "queue"));
  // Paths relative to cwd=FUZZ for shorter UI.
  const iFlag = resume ? "-" : "corpus";
  const argv = [
    aflFuzz,
    "-i",
    iFlag,
    "-o",
    "afl-out",
    role,
    name,
    "-G",
    String(maxLen),
    // HEIC decode under ASan can be slow (grids, 10-bit). Fixed 5s — auto
    // scaling with '+' often drops below the slowest dry-run cases.
    "-t",
    "5000",
    "-m",
    "none",
    ...(timeLimitSec > 0 ? ["-V", String(timeLimitSec)] : []),
    "--",
    exe,
  ];
  return Bun.spawn(argv, {
    cwd: FUZZ,
    env: quiet ? { ...env, AFL_NO_UI: "1" } : env,
    stdout: quiet ? "ignore" : "inherit",
    stderr: quiet ? "ignore" : "inherit",
  });
}

// Always name the primary `-M main` so scaling from 1 → N jobs resumes cleanly
// (AFL++ auto-names an unnamed instance `-S default`, which would orphan).
children.push(spawnAfl("-M", "main", false));
for (let j = 1; j < jobs; j++) {
  const id = `secondary-${String(j).padStart(2, "0")}`;
  children.push(spawnAfl("-S", id, true));
}

const codes = await Promise.all(children.map((c) => c.exited));
const rc = codes.find((c) => c !== 0) ?? 0;

// Always merge discoveries back into the shared corpus/crashes so libFuzzer
// and git-tracked regressions see them.
const imported = importAflFindings();
if (imported.queue > 0 || imported.crashes > 0) {
  console.log(
    `\nmerged into shared dirs: ${imported.queue} corpus input(s), ` +
      `${imported.crashes} crash(es)`,
  );
}

const fresh = listFiles(CRASHES).filter((f) => !beforeCrashes.has(f));
if (fresh.length > 0) {
  console.log(`\n${fresh.length} new crash artifact(s) in fuzz/crashes:`);
  for (const f of fresh) {
    console.log(`  ${f}`);
    console.log(`    reproduce: ${exe} ${join(CRASHES, f)}`);
    console.log(
      `    or:        bun cmd/fuzz-afl.ts -repro ${join("fuzz", "crashes", f)}`,
    );
    console.log(
      `    or:        bun cmd/fuzz.ts -repro ${join("fuzz", "crashes", f)}`,
    );
  }
  process.exit(1);
}

process.exit(rc);
