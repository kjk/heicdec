// bench.ts -- benchmark our decoder against strukturag libheif (native).
//
//   bun cmd/bench.ts <file.heic ... | -rand N | -all> [-clang] [-clean]
//   bun cmd/bench.ts -list-files
//
// Builds the oracle stack (libde265 + dav1d + libheif) and heic_test with
// -libheif, then runs `heic_test -bench` on each selected file. Session
// benchmark: open from memory, decode primary RGB, close; 2 interleaved runs
// each side; prints comparison table (op | libheif | heic | diff | %diff;
// + = heic slower). Same shape as djvudec's libdjvu bench.
//
// With no selection it prints usage + the available corpus file count.
import { dirname } from "path";
import { getDeps } from "./get-deps";
import { build, buildRef, cleanBuildOutput, defaultUseClang } from "./build";
import { corpusFiles, corpusSummary, fileLabel, selectFiles } from "./corpus";

const ROOT = dirname(import.meta.dir).replaceAll("\\", "/");

async function main(): Promise<void> {
  const argv = process.argv.slice(2);
  const useClang =
    argv.includes("-clang") ? true : argv.includes("-msvc") ? false : defaultUseClang;
  const doClean = argv.includes("-clean");

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
  -clang          build with clang instead of MSVC
  -clean          delete out/ first (forces full rebuild of harness + oracle)

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
  for (const file of files) {
    console.log(`${files.length > 1 ? "\n=== " : ""}${fileLabel(file, ROOT)}`);
    const r = Bun.spawnSync({
      cmd: [exe, "-bench", file],
      stdout: "pipe",
      stderr: "pipe",
      cwd: ROOT,
    });
    const out = (r.stdout?.toString() ?? "") + (r.stderr?.toString() ?? "");
    process.stdout.write(out);
    if (out.includes("skip: both heic and libheif failed")) {
      n_skip++;
      continue;
    }
    /* heic ok, libheif reject (e.g. double dimg iref) — timings only, not a fail */
    if (
      out.includes("libheif failed; heic timings only") ||
      out.includes("no compare")
    ) {
      n_skip++;
      continue;
    }
    if (r.exitCode) {
      n_fail++;
      rc = r.exitCode ?? 1;
    } else {
      n_ok++;
    }
  }
  if (files.length > 1) {
    console.log(`\nbench summary: ok=${n_ok} skip=${n_skip} fail=${n_fail}`);
  }
  process.exit(rc);
}

if (import.meta.main) await main();
