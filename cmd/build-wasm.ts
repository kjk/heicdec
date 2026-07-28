// build-wasm.ts — build a WebAssembly drop into dist/wasm/.
//
//   bun cmd/build-wasm.ts            # incremental build (bootstraps emsdk once)
//   bun cmd/build-wasm.ts -clean     # also wipe/re-activate the local emsdk
//   bun cmd/build-wasm.ts -dist      # compile dist/heic.c (after build-dist)
//
// Output: dist/wasm/heic.js + heic.wasm — Emscripten loader and binary.
// Serve dist/wasm over HTTP; browsers do not reliably fetch wasm from file://.
//
// Pure-C HEVC + unci (no dav1d / zlib / brotli). AVIF returns a clear error
// until a consumer links dav1d. Emscripten is bootstrapped into deps/emsdk
// when `emcc` is missing (same pattern as djvudec).
//
// build-dist.ts calls buildWasm({ useDist: true }) after amalgamation.

import { spawnSync } from "node:child_process";
import {
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import path from "node:path";

const ROOT = path.resolve(import.meta.dir, "..");
const SRC = path.join(ROOT, "src");
const DIST_C = path.join(ROOT, "dist", "heic.c");
const WASM = path.join(ROOT, "dist", "wasm");
export const WASM_JS = path.join(WASM, "heic.js");
export const WASM_BINARY = path.join(WASM, "heic.wasm");
const EMSDK = path.join(ROOT, "deps", "emsdk");
const isWin = process.platform === "win32";
const EMSDK_ENV = path.join(EMSDK, isWin ? "emsdk_env.bat" : "emsdk_env.sh");

/* emcc/binaryen shell out more reliably with POSIX slashes on Windows. */
const emPath = (p: string) => p.replaceAll("\\", "/");

const q = (s: string) => {
  const p = emPath(s);
  if (isWin) return /[\s"]/.test(p) ? `"${p.replace(/"/g, '""')}"` : p;
  return `'${p.replace(/'/g, `'\\''`)}'`;
};

function sh(
  cmd: string,
  opts: { cwd?: string; allowFail?: boolean; quiet?: boolean } = {},
) {
  const cwd = opts.cwd ?? ROOT;
  const stdio = opts.quiet ? ("pipe" as const) : ("inherit" as const);
  /* A git-bash parent leaks MSYSTEM, which makes emsdk print export lines
     instead of setting cmd.exe PATH. */
  const env = { ...process.env };
  delete env.MSYSTEM;
  const r = isWin
    ? spawnSync("cmd.exe", ["/d", "/s", "/c", cmd], {
        cwd,
        stdio,
        encoding: "utf8",
        env,
      })
    : spawnSync("bash", ["-lc", cmd], { cwd, stdio, encoding: "utf8", env });
  if (!opts.allowFail && r.status !== 0) {
    if (opts.quiet) process.stderr.write(r.stderr ?? "");
    throw new Error(`command failed (${r.status}): ${cmd}`);
  }
  return r;
}

function emccOnPath(): boolean {
  if (isWin) {
    return (
      spawnSync("where", ["emcc"], { shell: true, encoding: "utf8" }).status ===
      0
    );
  }
  return (
    spawnSync("bash", ["-lc", "command -v emcc"], { encoding: "utf8" })
      .status === 0
  );
}

function findEmcc(): string | null {
  if (emccOnPath()) return "";
  if (!existsSync(EMSDK_ENV)) return null;
  if (isWin) {
    const env = { ...process.env };
    delete env.MSYSTEM;
    const r = spawnSync(
      "cmd.exe",
      ["/d", "/s", "/c", `call ${q(EMSDK_ENV)} >nul 2>&1 && where emcc`],
      { encoding: "utf8", env },
    );
    if (r.status === 0) return `call ${q(EMSDK_ENV)} >nul 2>&1 && `;
  } else {
    const r = spawnSync(
      "bash",
      ["-lc", `source ${q(EMSDK_ENV)} >/dev/null 2>&1 && command -v emcc`],
      { encoding: "utf8" },
    );
    if (r.status === 0)
      return `source ${q(EMSDK_ENV)} >/dev/null 2>&1 && `;
  }
  return null;
}

function bootstrapEmsdk() {
  console.log(
    "• emcc not found — bootstrapping emsdk into deps/emsdk (one-time)…",
  );
  mkdirSync(path.dirname(EMSDK), { recursive: true });
  if (!existsSync(path.join(EMSDK, ".git"))) {
    sh(
      `git clone --depth 1 https://github.com/emscripten-core/emsdk.git ${q(EMSDK)}`,
    );
  }
  const emsdk = isWin ? q(path.join(EMSDK, "emsdk.bat")) : "./emsdk";
  sh(`${emsdk} install latest`, { cwd: EMSDK });
  sh(`${emsdk} activate latest`, { cwd: EMSDK });
}

function ensureEmcc(): string {
  let prefix = findEmcc();
  if (prefix === null) {
    bootstrapEmsdk();
    prefix = findEmcc();
    if (prefix === null)
      throw new Error("emsdk bootstrap did not produce a working emcc");
  }
  const v = sh(`${prefix}emcc --version`, { quiet: true });
  console.log("• using " + (v.stdout ?? "").split("\n")[0]);
  return prefix;
}

/* C entry points the web app calls, plus malloc/free for the input buffer. */
const EXPORTS = [
  "_heic_init",
  "_heic_ctx_new",
  "_heic_ctx_free",
  "_heic_doc_open",
  "_heic_doc_close",
  "_heic_doc_kind",
  "_heic_doc_info",
  "_heic_doc_decode",
  "_heic_image_destroy",
  "_malloc",
  "_free",
];

const RUNTIME = ["HEAP8", "HEAPU8", "HEAP32", "HEAPU32"];

function wasmInputMtime(useDist: boolean): number {
  if (useDist) {
    if (!existsSync(DIST_C))
      throw new Error("dist/heic.c missing — run bun cmd/build-dist.ts first");
    return statSync(DIST_C).mtimeMs;
  }
  let newest = 0;
  for (const f of readdirSync(SRC)) {
    if (!f.endsWith(".c") && !f.endsWith(".h") && !f.endsWith(".inc")) continue;
    newest = Math.max(newest, statSync(path.join(SRC, f)).mtimeMs);
  }
  return newest;
}

export function wasmOutdated(useDist = false): boolean {
  if (!existsSync(WASM_JS) || !existsSync(WASM_BINARY)) return true;
  const inputMtime = wasmInputMtime(useDist);
  return (
    statSync(WASM_JS).mtimeMs < inputMtime ||
    statSync(WASM_BINARY).mtimeMs < inputMtime
  );
}

function compile(prefix: string, useDist: boolean) {
  mkdirSync(WASM, { recursive: true });
  /* Delete prior outputs so emcc/wasm-opt create fresh files instead of
     rewriting in place (Windows: "Failed opening output file … Invalid argument"). */
  rmSync(WASM_JS, { force: true });
  rmSync(WASM_BINARY, { force: true });
  const out = q(WASM_JS);
  const flags = [
    "-O2",
    "-sMODULARIZE=1",
    "-sEXPORT_NAME=createHeicModule",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sENVIRONMENT=web",
    "-sEXPORT_ES6=0",
    "-sINCOMING_MODULE_JS_API=wasmBinary",
    /* HEVC stills can be multi‑MP; default INITIAL_MEMORY is tight. */
    "-sINITIAL_MEMORY=67108864",
    `-sEXPORTED_FUNCTIONS=${EXPORTS.join(",")}`,
    `-sEXPORTED_RUNTIME_METHODS=${RUNTIME.join(",")}`,
  ].join(" ");

  let inputs: string;
  if (useDist) {
    inputs = q(DIST_C);
    console.log("• compiling dist/heic.c → dist/wasm/heic.js");
  } else {
    const cfiles = readdirSync(SRC)
      .filter((f) => f.endsWith(".c"))
      .map((f) => q(path.join(SRC, f)))
      .join(" ");
    inputs = `${cfiles} -I ${q(SRC)}`;
    const n = readdirSync(SRC).filter((f) => f.endsWith(".c")).length;
    console.log(`• compiling ${n} C files → dist/wasm/heic.js`);
  }

  sh(`${prefix}emcc ${inputs} ${flags} -o ${out}`);
  // emcc on Windows may emit CRLF in the JS glue; keep the committed drop LF-only
  // (matches .gitattributes eol=lf and avoids noisy line-ending diffs).
  {
    const raw = readFileSync(WASM_JS);
    if (raw.includes(0x0d)) {
      writeFileSync(
        WASM_JS,
        Buffer.from(raw.toString("utf8").replace(/\r\n/g, "\n")),
      );
    }
  }
  const kb = (Bun.file(WASM_JS).size / 1024).toFixed(0);
  const wasmKb = (Bun.file(WASM_BINARY).size / 1024).toFixed(0);
  console.log(
    `✓ wrote dist/wasm/heic.js (${kb} KB) + heic.wasm (${wasmKb} KB)`,
  );
  console.log(
    "  run the demo: bun cmd/run-wasm-demo.ts → http://localhost:8000/",
  );
  console.log(
    "  note: pure-C HEVC/unci only; AV1 (avif) needs dav1d (not in this wasm drop)",
  );
}

export function buildWasm(
  opts: { useDist?: boolean; cleanEmsdk?: boolean } = {},
): void {
  const useDist = opts.useDist ?? false;
  if (opts.cleanEmsdk) rmSync(EMSDK, { recursive: true, force: true });
  const prefix = ensureEmcc();
  compile(prefix, useDist);
}

export function ensureWasm(useDist = false): void {
  if (!wasmOutdated(useDist)) {
    console.log("dist/wasm/ up to date");
    return;
  }
  buildWasm({ useDist });
}

if (import.meta.main) {
  const clean = process.argv.includes("-clean");
  const useDist = process.argv.includes("-dist");
  buildWasm({ cleanEmsdk: clean, useDist });
}
