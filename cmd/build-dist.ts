// build-dist.ts -- SQLite-style amalgamation in dist/.
//
//   bun cmd/build-dist.ts
//
// Emits:
//   dist/heic.h + dist/heic.c  — amalgamation (does NOT include dav1d)
//   dist/wasm/heic.{js,wasm}   — Emscripten loader and binary from dist/heic.c
//                                for the demo at dist/wasm/demo.html
import { $ } from "bun";
import {
  readFileSync,
  writeFileSync,
  mkdirSync,
  existsSync,
  statSync,
  rmSync,
} from "fs";
import { join } from "path";
import { clangCFlags, HEIC_MSVC_CL_C, isWindows, SRCS } from "./build";
import { buildWasm } from "./build-wasm";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const SRC = join(ROOT, "src");
const DIST = join(ROOT, "dist");

export const DIST_MODULES = SRCS.map((s) => s.replace(/^src\//, ""));
export const DIST_H = join(DIST, "heic.h");
export const DIST_C = join(DIST, "heic.c");

/* Source fragments included by a module must be embedded too: dist/heic.c is
 * deliberately standalone and cannot depend on files left under src/.
 * Headers that would duplicate symbols if pasted into every .c are stripped
 * and injected once after heic_internal.h (see ONCE_HEADERS). */
const EMBEDDED_INCLUDES = ["hevc_cabac_init.inc"];
const ONCE_HEADERS = ["hevc_cabac_inline.h"];
const STRIP_LOCAL_HEADERS = [
  "heic_internal\\.h",
  "heic\\.h",
  "hevc_cabac_inline\\.h",
];

function stripIncludes(text: string, headers: string[]): string {
  const re = new RegExp(
    `^[ \\t]*#[ \\t]*include[ \\t]+"(?:${headers.join("|")})"[ \\t]*\\r?\\n`,
    "gm",
  );
  return text.replace(re, "");
}

function embedIncludes(text: string): string {
  for (const name of EMBEDDED_INCLUDES) {
    const re = new RegExp(
      `^[ \\t]*#[ \\t]*include[ \\t]+"${name.replace(".", "\\.")}"[ \\t]*$`,
      "gm",
    );
    const included = readFileSync(join(SRC, name), "utf8").trimEnd();
    text = text.replace(re, included);
  }
  return text;
}

/** LF only, strip trailing whitespace, at most one blank line in a row. */
function normalizeSourceText(text: string): string {
  const lines = text.replace(/\r\n/g, "\n").replace(/\r/g, "\n").split("\n");
  const out: string[] = [];
  let blank = false;
  for (const raw of lines) {
    const line = raw.replace(/[ \t]+$/, "");
    if (line.length === 0) {
      if (blank) continue;
      blank = true;
      out.push("");
    } else {
      blank = false;
      out.push(line);
    }
  }
  // Drop leading blank lines; keep a single trailing newline.
  while (out.length > 0 && out[0] === "") out.shift();
  while (out.length > 0 && out[out.length - 1] === "") out.pop();
  return out.length === 0 ? "\n" : out.join("\n") + "\n";
}

function stripCComments(code: string): string {
  // Normalize EOLs first so comment/blank handling never sees CRLF.
  code = code.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  let out = "";
  let i = 0;
  const n = code.length;
  while (i < n) {
    const c = code[i];
    const next = i + 1 < n ? code[i + 1] : "";
    if (c === '"') {
      out += c;
      i++;
      while (i < n) {
        if (code[i] === "\\" && i + 1 < n) {
          out += code[i] + code[i + 1];
          i += 2;
        } else if (code[i] === '"') {
          out += code[i];
          i++;
          break;
        } else {
          out += code[i];
          i++;
        }
      }
      continue;
    }
    if (c === "'") {
      out += c;
      i++;
      while (i < n) {
        if (code[i] === "\\" && i + 1 < n) {
          out += code[i] + code[i + 1];
          i += 2;
        } else if (code[i] === "'") {
          out += code[i];
          i++;
          break;
        } else {
          out += code[i];
          i++;
        }
      }
      continue;
    }
    if (c === "/" && next === "/") {
      i += 2;
      while (i < n && code[i] !== "\n") i++;
      continue;
    }
    if (c === "/" && next === "*") {
      i += 2;
      while (i + 1 < n && !(code[i] === "*" && code[i + 1] === "/")) i++;
      i += 2;
      continue;
    }
    out += c;
    i++;
  }
  return normalizeSourceText(out);
}

async function runCmd(cmd: string, cwd?: string): Promise<number> {
  console.log(cwd ? `+ cd ${cwd} && ${cmd}` : `+ ${cmd}`);
  const shell = $`${{ raw: cmd }}`.nothrow();
  if (cwd) await shell.cwd(cwd);
  const r = await shell;
  return r.exitCode ?? 1;
}

export async function buildDist(): Promise<void> {
  mkdirSync(DIST, { recursive: true });
  const publicHeader = normalizeSourceText(readFileSync(join(SRC, "heic.h"), "utf8"));
  writeFileSync(DIST_H, publicHeader);

  const parts: string[] = [publicHeader];
  const internal = readFileSync(join(SRC, "heic_internal.h"), "utf8");
  parts.push(stripIncludes(internal, ["heic\\.h"]));
  /* Inline once: residual/ctu/cabac all #include this; embedding per .c would
   * redeclare static symbols in the amalgamation. */
  for (const name of ONCE_HEADERS) {
    const once = readFileSync(join(SRC, name), "utf8");
    parts.push(stripIncludes(once, ["heic_internal\\.h", "heic\\.h"]));
  }

  for (const name of DIST_MODULES) {
    const code = readFileSync(join(SRC, name), "utf8");
    parts.push(embedIncludes(stripIncludes(code, STRIP_LOCAL_HEADERS)));
  }

  // Final pass also collapses blank runs left at module boundaries.
  const amalgamated = stripCComments(parts.join("\n"));
  writeFileSync(DIST_C, amalgamated);
  console.log(`wrote dist/heic.h (${publicHeader.split("\n").length} lines)`);
  console.log(
    `wrote dist/heic.c (${amalgamated.split("\n").length} lines, ${DIST_MODULES.length} modules)`,
  );

  const rc = await runCmd(
    `clang ${clangCFlags("-O1")} -c dist/heic.c -o dist/heic_verify.o`,
    ROOT,
  );
  if (existsSync(join(DIST, "heic_verify.o"))) rmSync(join(DIST, "heic_verify.o"));
  if (rc !== 0) {
    console.error("amalgamation FAILED to compile (clang)");
    process.exit(1);
  }
  console.log("amalgamation compiles cleanly (clang) ✓");

  if (isWindows) {
    const r2 = await runCmd(
      `cl ${HEIC_MSVC_CL_C} -c dist/heic.c -Fodist/heic_verify.obj`,
      ROOT,
    );
    if (existsSync(join(DIST, "heic_verify.obj")))
      rmSync(join(DIST, "heic_verify.obj"));
    if (r2 !== 0) {
      console.error("amalgamation FAILED to compile (msvc)");
      process.exit(1);
    }
    console.log("amalgamation compiles cleanly (msvc) ✓");
  }

  buildWasm({ useDist: true });
}

if (import.meta.main) {
  await buildDist();
}
