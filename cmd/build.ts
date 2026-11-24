// build.ts -- build driver for the C HEIC decoder.
//
//   bun cmd/build.ts          deps + harness (MSVC default on Windows)
//   bun cmd/build.ts -clang
//   bun cmd/build.ts -clean
import { $ } from "bun";
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readdirSync,
  readFileSync,
  rmSync,
  statSync,
  writeFileSync,
} from "fs";
import { resolve as resolvePath } from "path";
import { getDeps } from "./get-deps";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const OUT_ROOT = `${ROOT}/out`;

export const isWindows = process.platform === "win32";
export const isMac = process.platform === "darwin";
export const defaultUseClang = !isWindows;

const outDir = (useClang: boolean) =>
  `${OUT_ROOT}/${useClang ? "clang" : "msvc"}`;

function binName(base: string): string {
  return isWindows ? `${base}.exe` : base;
}

// Every header a translation unit can include (src/*.h, src/*.inc, test/*.h).
// Tracking only heic.h + heic_internal.h left stale objects after edits to
// hevc_cabac_inline.h / hevc_cabac_init.inc / the harness headers.
function trackedHeaders(): string[] {
  const out: string[] = [];
  for (const dir of ["src", "test"]) {
    for (const name of readdirSync(`${ROOT}/${dir}`)) {
      if (/\.(h|inc)$/.test(name)) out.push(`${ROOT}/${dir}/${name}`);
    }
  }
  return out;
}

// Translation units (order matches amalgamation).
export const SRCS = [
  "src/core.c",
  "src/heif.c",
  "src/document.c",
  "src/decode.c",
  "src/color.c",
  "src/hevc_bitstream.c",
  "src/hevc_params.c",
  "src/hevc_cabac.c",
  "src/hevc_slice.c",
  "src/hevc_residual.c",
  "src/hevc_transform.c",
  "src/hevc_simd.c",
  "src/hevc_intra.c",
  "src/hevc_inter.c",
  "src/hevc_ctu.c",
  "src/hevc_sao.c",
  "src/hevc_deblock.c",
  "src/hevc_decode.c",
  "src/av1_dav1d.c",
  "src/unci.c",
];

const objBase = (src: string) =>
  src.replace(/^src\//, "").replace(/\.c$/, "");

function needsRebuild(output: string, ...inputs: string[]): boolean {
  if (!existsSync(output)) return true;
  const outMtime = statSync(output).mtimeMs;
  for (const input of inputs) {
    if (!existsSync(input)) return true;
    if (statSync(input).mtimeMs > outMtime) return true;
  }
  return false;
}

async function runCmd(cmd: string, cwd?: string): Promise<void> {
  console.log(cwd ? `+ cd ${cwd} && ${cmd}` : `+ ${cmd}`);
  const shell = $`${{ raw: cmd }}`;
  if (cwd) await shell.cwd(cwd);
  else await shell;
}

export function cleanBuildOutput(): void {
  rmSync(OUT_ROOT, { recursive: true, force: true });
}

/* -Zi: CodeView for .pdb (winperf / VS). -O2 keeps release-ish speed for bench. */
export const MSVC_CL_COMMON = `-nologo -O2 -Ob3 -Zi -MT`;
export const HEIC_MSVC_CL_C =
  `${MSVC_CL_COMMON} -W4 -WX -std:c11 -D_CRT_SECURE_NO_WARNINGS`;

export const HEIC_CLANG_C_WARN =
  "-Wall -Wextra -Wuninitialized -Winit-self -Werror";
const HEIC_CLANG_C_STD = "-std=c11";

/** Debug info: DWARF on *nix; CodeView PDB on Windows (winperf).
 *  hevc_simd.c uses SSE4.1/SSSE3 intrinsics; clang requires -msse4.1 so
 *  always_inline intrinsics may be inlined into the TU (MSVC does not). */
export function clangCFlags(opt = "-g -O3", win = isWindows): string {
  const crt = win ? " -D_CRT_SECURE_NO_WARNINGS" : "";
  const pdb = win ? " -gcodeview" : "";
  const sse =
    process.arch === "x64" || process.arch === "ia32" ? " -msse4.1" : "";
  return `${HEIC_CLANG_C_STD} ${opt}${pdb}${sse} ${HEIC_CLANG_C_WARN}${crt}`;
}

type CompileUnit = { src: string; obj: string; label: string };

function cUnits(dir: string, ext: string, withLibheif: boolean): CompileUnit[] {
  const units: CompileUnit[] = [
    ...SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${dir}/${objBase(s)}.${ext}`,
      label: s,
    })),
    {
      src: `${ROOT}/test/heic_test.c`,
      obj: `${dir}/heic_test.${ext}`,
      label: "test/heic_test.c",
    },
  ];
  if (withLibheif) {
    units.push({
      src: `${ROOT}/test/bench_libheif.c`,
      obj: `${dir}/bench_libheif.${ext}`,
      label: "test/bench_libheif.c",
    });
  }
  /* WIC bench is Windows-only; always compile the stub/real unit on win so
   * heic_test.c can call heic_bench_wic_session when _WIN32 is set. */
  if (isWindows) {
    units.push({
      src: `${ROOT}/test/bench_wic.c`,
      obj: `${dir}/bench_wic.${ext}`,
      label: "test/bench_wic.c",
    });
  }
  return units;
}

/** System libs for WIC HEIF/AVIF decode timing (Windows only). */
function wicLinkLibs(): string[] {
  if (!isWindows) return [];
  return ["windowscodecs.lib", "ole32.lib", "shlwapi.lib"];
}

const harnessExeName = (useClang: boolean) =>
  binName(`heic_test_${useClang ? "clang" : "msvc"}`);

export type BuildOpts = {
  withDav1d?: boolean;
  /** Link strukturag libheif + libde265 (+ dav1d) for -bench / -verify. */
  withLibheif?: boolean;
};

/** Oracle static libs built into out/ (cmake/meson). */
export type LibheifPaths = {
  heifInc: string[];
  heifLib: string;
  de265Lib: string;
  dav1dLib: string;
  dav1dInc: string[];
};

export function libheifPaths(): LibheifPaths | null {
  const heifLib = `${ROOT}/out/libheif_build/libheif/heif.lib`;
  const heifLibA = `${ROOT}/out/libheif_build/libheif/libheif.a`;
  const de265Lib = `${ROOT}/out/libde265_build/libde265/libde265.lib`;
  const de265LibA = `${ROOT}/out/libde265_build/libde265/libde265.a`;
  const lib = existsSync(heifLib) ? heifLib : existsSync(heifLibA) ? heifLibA : null;
  const de265 = existsSync(de265Lib)
    ? de265Lib
    : existsSync(de265LibA)
      ? de265LibA
      : null;
  const dav1d = dav1dPaths();
  if (!lib || !de265 || !dav1d) return null;
  const heifInc = [
    `${ROOT}/deps/libheif/libheif/api`,
    `${ROOT}/out/libheif_build`, // libheif/heif_version.h
  ];
  return {
    heifInc,
    heifLib: lib,
    de265Lib: de265,
    dav1dLib: dav1d.lib,
    dav1dInc: dav1d.inc,
  };
}

/** Static zlib built into out/zlib_build (unci deflate/zlib). */
export type ZlibPaths = { inc: string[]; lib: string };
export function zlibPaths(): ZlibPaths | null {
  const inc = [`${ROOT}/deps/zlib`, `${ROOT}/out/zlib_build`];
  const libs = [
    `${ROOT}/out/zlib_build/libzs.lib`, // Windows static (zlib cmake)
    `${ROOT}/out/zlib_build/libz.a`,
    `${ROOT}/out/zlib_build/zlibstatic.lib`,
    `${ROOT}/out/zlib_build/libz.lib`,
  ];
  for (const lib of libs) {
    if (existsSync(lib) && existsSync(`${ROOT}/deps/zlib/zlib.h`)) {
      return { inc, lib };
    }
  }
  return null;
}

/** Static brotli (common+dec; enc only needed by libheif). */
export type BrotliPaths = {
  inc: string[];
  common: string;
  dec: string;
  enc: string;
};
export function brotliPaths(): BrotliPaths | null {
  const inc = [`${ROOT}/deps/brotli/c/include`];
  const build = `${ROOT}/out/brotli_build`;
  const commonCands = [
    `${build}/brotlicommon.lib`,
    `${build}/libbrotlicommon.a`,
  ];
  const decCands = [`${build}/brotlidec.lib`, `${build}/libbrotlidec.a`];
  const encCands = [`${build}/brotlienc.lib`, `${build}/libbrotlienc.a`];
  const common = commonCands.find((p) => existsSync(p));
  const dec = decCands.find((p) => existsSync(p));
  const enc = encCands.find((p) => existsSync(p));
  if (!common || !dec || !enc || !existsSync(`${inc[0]}/brotli/decode.h`))
    return null;
  return { inc, common, dec, enc };
}

/** Paths for an optional prebuilt/static dav1d. Override with env. */
export function dav1dPaths(): { inc: string[]; lib: string } | null {
  const envInc = process.env.HEIC_DAV1D_INC;
  const envLib = process.env.HEIC_DAV1D_LIB;
  if (envInc && envLib && existsSync(envInc) && existsSync(envLib)) {
    return {
      inc: [envInc.replaceAll("\\", "/")],
      lib: envLib.replaceAll("\\", "/"),
    };
  }
  const srcInc = `${ROOT}/deps/dav1d/include`;
  const buildInc = `${ROOT}/out/dav1d_build/include`;
  const libs = [
    `${ROOT}/out/dav1d_build/src/libdav1d.a`,
    `${ROOT}/out/dav1d_build/src/libdav1d.lib`,
    `${ROOT}/out/dav1d_build/src/dav1d.lib`,
  ];
  if (!existsSync(srcInc)) return null;
  for (const lib of libs) {
    if (existsSync(lib)) {
      const inc = [srcInc];
      if (existsSync(buildInc)) inc.unshift(buildInc);
      return { inc, lib };
    }
  }
  return null;
}

function dav1dIncFlags(dav1d: { inc: string[] }): string {
  return dav1d.inc.map((p) => ` -I${p}`).join("");
}

/**
 * Objects live in one directory per configuration (feature set + exact
 * compile command), so alternating between e.g. `build.ts`, `tests.ts`
 * (libheif oracle, /MD) and `verify-release.ts` (dav1d only) no longer
 * deletes and recompiles every object each time, and a changed compiler flag
 * cannot leave stale objects behind. `features` names the directory;
 * `compileCmd` is hashed into it.
 */
function objDirFor(dir: string, features: string, compileCmd: string): string {
  const slug = features.replace(/[^A-Za-z0-9]+/g, "-").replace(/^-|-$/g, "");
  const hash = Bun.hash(compileCmd).toString(16).slice(0, 8);
  const objDir = `${dir}/obj-${slug}-${hash}`;
  mkdirSync(objDir, { recursive: true });
  return objDir;
}

/**
 * The harness exe is shared by every configuration: when the configuration
 * that last linked it differs, drop it (and its pdb) so it is relinked.
 */
function syncExeStamp(dir: string, useClang: boolean, want: string): void {
  const stamp = `${dir}/.heic_features`;
  const prev = existsSync(stamp) ? readFileSync(stamp, "utf8").trim() : "";
  if (prev === want) return;
  const exe = `${dir}/${harnessExeName(useClang)}`;
  for (const f of [exe, exe.replace(/(\.exe)?$/, ".pdb")])
    rmSync(f, { force: true });
  mkdirSync(dir, { recursive: true });
  writeFileSync(stamp, want);
}

/** Run `jobs` with at most `limit` in flight. */
async function runPool<T>(
  items: T[],
  limit: number,
  job: (item: T) => Promise<void>,
): Promise<void> {
  let next = 0;
  const worker = async () => {
    while (next < items.length) await job(items[next++]!);
  };
  await Promise.all(
    Array.from({ length: Math.max(1, Math.min(limit, items.length)) }, worker),
  );
}

function compressFeatureFlags(
  zlib: ZlibPaths | null,
  brotli: BrotliPaths | null,
): { def: string; inc: string; libs: string[] } {
  let def = "";
  let inc = "";
  const libs: string[] = [];
  if (zlib) {
    def += " -DHEIC_HAVE_ZLIB";
    for (const p of zlib.inc) inc += ` -I${p}`;
    libs.push(zlib.lib);
  }
  if (brotli) {
    def += " -DHEIC_HAVE_BROTLI";
    for (const p of brotli.inc) inc += ` -I${p}`;
    /* decoder only for our unci path; order: dec then common */
    libs.push(brotli.dec, brotli.common);
  }
  return { def, inc, libs };
}

async function buildClang(opts: BuildOpts = {}): Promise<string> {
  const dir = outDir(true);
  const exePath = `${dir}/${harnessExeName(true)}`;
  mkdirSync(dir, { recursive: true });
  const withLibheif = !!opts.withLibheif;
  const needDav1d = !!opts.withDav1d || withLibheif;
  const dav1d = needDav1d ? dav1dPaths() : null;
  const heif = withLibheif ? libheifPaths() : null;
  const zlib = zlibPaths();
  const brotli = brotliPaths();
  if (needDav1d && !dav1d) {
    throw new Error(
      "dav1d requested but not found. Run bun cmd/build.ts -build-dav1d first",
    );
  }
  if (withLibheif && !heif) {
    throw new Error(
      "libheif requested but not found. Run bun cmd/build.ts -build-libheif first",
    );
  }
  const comp = compressFeatureFlags(zlib, brotli);
  const features =
    `dav1d=${needDav1d ? 1 : 0};libheif=${withLibheif ? 1 : 0};zlib=${zlib ? 1 : 0};brotli=${brotli ? 1 : 0}`;
  let def = comp.def;
  let inc = ` -I${ROOT}/src -I${ROOT}/test` + comp.inc;
  if (dav1d) {
    def += " -DHEIC_HAVE_DAV1D";
    inc += dav1dIncFlags(dav1d);
  }
  if (heif) {
    def +=
      " -DHEIC_HAVE_LIBHEIF -DLIBHEIF_STATIC_BUILD -DLIBDE265_STATIC_BUILD";
    for (const p of heif.heifInc) inc += ` -I${p}`;
  }
  const clangC = `clang ${clangCFlags()}${def}${inc} -c`;
  const objDir = objDirFor(dir, features, clangC);
  syncExeStamp(dir, true, objDir);
  const units = cUnits(objDir, "o", withLibheif);
  const hdrs = trackedHeaders();
  await runPool(
    units.filter((u) => needsRebuild(u.obj, u.src, ...hdrs)),
    navigator.hardwareConcurrency || 4,
    (u) => runCmd(`${clangC} -o ${u.obj} ${u.src}`),
  );
  const objs = units.map((u) => u.obj);
  const linkLibs: string[] = [];
  if (heif) {
    linkLibs.push(heif.heifLib, heif.de265Lib, heif.dav1dLib);
    /* libheif may reference brotli enc when HAVE_BROTLI */
    if (brotli) linkLibs.push(brotli.enc, brotli.dec, brotli.common);
    if (zlib) linkLibs.push(zlib.lib);
  } else {
    if (dav1d) linkLibs.push(dav1d.lib);
    linkLibs.push(...comp.libs);
  }
  if (!isWindows) linkLibs.push("-lpthread", "-lm");
  /* The clang driver wants -lfoo: a bare foo.lib is treated as an input file
     path and fails with "no such file or directory". */
  else linkLibs.push(...wicLinkLibs().map((l) => `-l${l.replace(/\.lib$/, "")}`));
  const linkExtra = linkLibs.length ? ` ${linkLibs.join(" ")}` : "";
  const linkCmd = withLibheif ? "clang++" : "clang";
  /* Windows: -g + -gcodeview → heic_test_clang.pdb next to the exe. */
  const linkDbg = isWindows ? " -g -gcodeview" : " -g";
  if (
    needsRebuild(
      exePath,
      ...objs,
      ...(heif ? [heif.heifLib, heif.de265Lib, heif.dav1dLib] : dav1d ? [dav1d.lib] : []),
      ...comp.libs,
    )
  ) {
    await runCmd(
      `${linkCmd}${linkDbg} ${objs.join(" ")}${linkExtra} -o ${exePath}`,
    );
  }
  return exePath;
}

async function buildMsvc(opts: BuildOpts = {}): Promise<string> {
  if (!isWindows) throw new Error("MSVC build requires Windows");
  const dir = outDir(false);
  const exePath = `${dir}/${harnessExeName(false)}`;
  mkdirSync(dir, { recursive: true });
  const withLibheif = !!opts.withLibheif;
  const needDav1d = !!opts.withDav1d || withLibheif;
  const dav1d = needDav1d ? dav1dPaths() : null;
  const heif = withLibheif ? libheifPaths() : null;
  const zlib = zlibPaths();
  const brotli = brotliPaths();
  if (needDav1d && !dav1d) {
    throw new Error("dav1d requested but not found. Run -build-dav1d first");
  }
  if (withLibheif && !heif) {
    throw new Error("libheif requested but not found. Run -build-libheif first");
  }
  const comp = compressFeatureFlags(zlib, brotli);
  /* meson/cmake deps (dav1d, libheif, libde265) are /MD; match CRT when linking them. */
  const useMd = withLibheif || needDav1d;
  const features =
    `dav1d=${needDav1d ? 1 : 0};libheif=${withLibheif ? 1 : 0};zlib=${zlib ? 1 : 0};brotli=${brotli ? 1 : 0};crt=${useMd ? "MD" : "MT"}`;
  let def = comp.def;
  let inc = " -Isrc -Itest" + comp.inc;
  if (dav1d) {
    def += " -DHEIC_HAVE_DAV1D";
    inc += dav1dIncFlags(dav1d);
  }
  if (heif) {
    def +=
      " -DHEIC_HAVE_LIBHEIF -DLIBHEIF_STATIC_BUILD -DLIBDE265_STATIC_BUILD";
    for (const p of heif.heifInc) inc += ` -I${p}`;
  }
  const msvcC = useMd
    ? HEIC_MSVC_CL_C.replace(/\s-MT\b/, " -MD")
    : HEIC_MSVC_CL_C;
  const objDir = objDirFor(dir, features, `${msvcC}${def}${inc}`);
  syncExeStamp(dir, false, objDir);
  const units = cUnits(objDir, "obj", withLibheif);
  /* Type info for the objects; the linker writes heic_test_msvc.pdb next to
     the exe from it (winperf symbolicates from that one). */
  const pdb = `${dir}/heic_test_msvc.pdb`;
  const clC = `${msvcC}${def}${inc} -Fo${objDir}/ -Fd${objDir}/objs.pdb -c`;
  const hdrs = trackedHeaders();
  const stale = units
    .filter((u) => needsRebuild(u.obj, u.src, ...hdrs))
    .map((u) =>
      u.src.startsWith(`${ROOT}/`) ? u.src.slice(ROOT.length + 1) : u.src,
    );
  /* One cl for all stale units: -MP compiles them in parallel. */
  if (stale.length) await runCmd(`cl ${clC} -MP ${stale.join(" ")}`, ROOT);
  const objs = units.map((u) => u.obj);
  const linkLibs: string[] = [];
  if (heif) {
    linkLibs.push(heif.heifLib, heif.de265Lib, heif.dav1dLib);
    if (brotli) linkLibs.push(brotli.enc, brotli.dec, brotli.common);
    if (zlib) linkLibs.push(zlib.lib);
  } else {
    if (dav1d) linkLibs.push(dav1d.lib);
    linkLibs.push(...comp.libs);
  }
  linkLibs.push(...wicLinkLibs());
  const linkExtra = linkLibs.length ? ` ${linkLibs.join(" ")}` : "";
  if (
    needsRebuild(
      exePath,
      ...objs,
      ...(heif ? [heif.heifLib, heif.de265Lib, heif.dav1dLib] : dav1d ? [dav1d.lib] : []),
      ...comp.libs,
    )
  ) {
    /* -DEBUG → heic_test_msvc.pdb; oracle is C++ so pull MD C++ runtime. */
    const crt = useMd ? "-MD " : "";
    const cxxRt = withLibheif ? " -DEFAULTLIB:msvcprt" : "";
    const link = `cl -nologo ${crt}-Zi ${objs.join(" ")}${linkExtra} -Fe:${exePath} -link -DEBUG -PDB:${pdb}${cxxRt}`;
    await runCmd(link, ROOT);
  }
  return exePath;
}

export async function build(
  useClang: boolean = defaultUseClang,
  opts: BuildOpts = {},
): Promise<string> {
  await getDeps();
  if (useClang) return buildClang(opts);
  return buildMsvc(opts);
}

/** True if nasm is on PATH (needed for dav1d x86 asm). */
async function hasNasm(): Promise<boolean> {
  try {
    await $`nasm -v`.quiet();
    return true;
  } catch {
    /* Common Windows install location not always on PATH for meson. */
    const candidates = [
      "C:/Program Files/NASM/nasm.exe",
      "C:/Program Files (x86)/NASM/nasm.exe",
    ];
    for (const p of candidates) {
      if (existsSync(p)) {
        const dir = p.replace(/\/nasm\.exe$/i, "").replaceAll("/", "\\");
        process.env.PATH = `${dir};${process.env.PATH ?? ""}`;
        return true;
      }
    }
    return false;
  }
}

/**
 * Require an external tool on PATH. Do not search install dirs — if the user
 * has not put it on PATH, exit with a winget one-liner (Windows) or package name.
 * Does not return when missing (process.exit).
 */
async function requireTool(
  name: string,
  wingetId: string,
  extraHint?: string,
): Promise<void> {
  try {
    await $`${name} --version`.quiet();
  } catch {
    console.error(`${name} not found on PATH.`);
    if (isWindows) {
      console.error(`Install with:  winget install -e --id ${wingetId}`);
      console.error("Then open a new terminal so PATH is updated.");
    } else {
      console.error(`Install ${name} (e.g. brew/apt) and ensure it is on PATH.`);
    }
    if (extraHint) console.error(extraHint);
    process.exit(1);
  }
}

async function requireCmakeNinja(): Promise<void> {
  await requireTool("cmake", "Kitware.CMake");
  await requireTool("ninja", "Ninja-build.Ninja");
}

/** Build static libdav1d into out/dav1d_build (meson + ninja). */
export async function buildDav1d(opts: { force?: boolean; asm?: boolean } = {}): Promise<string> {
  await getDeps();
  await requireTool("meson", "mesonbuild.meson");
  await requireTool("ninja", "Ninja-build.Ninja");
  const src = `${ROOT}/deps/dav1d`;
  const buildDir = `${ROOT}/out/dav1d_build`;
  const libA = `${buildDir}/src/libdav1d.a`;
  const libLib = `${buildDir}/src/libdav1d.lib`;
  const wantAsm = opts.asm !== false && (await hasNasm());
  const stamp = `${buildDir}/.heic_dav1d_stamp`;
  const stampWant = `asm=${wantAsm ? 1 : 0}`;
  if (!opts.force && (existsSync(libA) || existsSync(libLib))) {
    const prev = existsSync(stamp) ? readFileSync(stamp, "utf8").trim() : "";
    if (prev === stampWant) {
      const lib = existsSync(libA) ? libA : libLib;
      console.log(`dav1d already built: ${lib} (${stampWant})`);
      return lib;
    }
    console.log(`dav1d stamp ${prev || "none"} → ${stampWant}; rebuilding...`);
  }
  if (existsSync(buildDir)) {
    rmSync(buildDir, { recursive: true, force: true });
  }
  mkdirSync(buildDir, { recursive: true });
  const asmFlag = wantAsm ? "true" : "false";
  console.log(`dav1d: enable_asm=${asmFlag}${wantAsm ? "" : " (nasm not found)"}`);
  await runCmd(
    `meson setup "${buildDir}" "${src}" --default-library=static ` +
      `-Denable_tools=false -Denable_tests=false -Denable_examples=false ` +
      `-Denable_asm=${asmFlag} -Denable_docs=false`,
  );
  await runCmd(`ninja -C "${buildDir}"`);
  writeFileSync(stamp, stampWant);
  if (existsSync(libA)) return libA;
  if (existsSync(libLib)) return libLib;
  throw new Error("dav1d build finished but libdav1d not found under out/dav1d_build/src");
}

/** Build static libde265 (HEVC oracle backend for libheif). */
export async function buildLibDe265(): Promise<string> {
  await getDeps();
  await requireCmakeNinja();
  const src = `${ROOT}/deps/libde265`;
  const buildDir = `${ROOT}/out/libde265_build`;
  const lib = `${buildDir}/libde265/libde265.lib`;
  const libA = `${buildDir}/libde265/libde265.a`;
  const syncVersionHeader = () => {
    /* de265.h includes this CMake-generated header. Use resolved paths because
     * Bun's Windows copyFileSync does not reliably handle a "/../" source. */
    const ver = resolvePath(buildDir, "libde265/de265-version.h");
    const verDst = resolvePath(src, "libde265/de265-version.h");
    if (existsSync(ver)) copyFileSync(ver, verDst);
  };
  if (existsSync(lib) || existsSync(libA)) {
    const p = existsSync(lib) ? lib : libA;
    syncVersionHeader();
    console.log(`libde265 already built: ${p}`);
    return p;
  }
  mkdirSync(buildDir, { recursive: true });
  await runCmd(
    `cmake -G Ninja -S "${src}" -B "${buildDir}" ` +
      `-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ` +
      `-DENABLE_SDL=OFF -DENABLE_ENCODER=OFF -DENABLE_DECODER=ON`,
  );
  await runCmd(`ninja -C "${buildDir}"`);
  syncVersionHeader();
  if (existsSync(lib)) return lib;
  if (existsSync(libA)) return libA;
  throw new Error("libde265 build finished but library not found");
}

/** Build static zlib for unci deflate/zlib (and libheif oracle). */
export async function buildZlib(): Promise<string> {
  await getDeps();
  await requireCmakeNinja();
  const src = `${ROOT}/deps/zlib`;
  const buildDir = `${ROOT}/out/zlib_build`;
  const existing = zlibPaths();
  if (existing) {
    console.log(`zlib already built: ${existing.lib}`);
    return existing.lib;
  }
  mkdirSync(buildDir, { recursive: true });
  await runCmd(
    `cmake -G Ninja -S "${src}" -B "${buildDir}" ` +
      `-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF`,
  );
  await runCmd(`ninja -C "${buildDir}"`);
  /* zlib.h includes "zconf.h"; cmake emits zconf.h into the build dir. */
  const zconf = `${buildDir}/zconf.h`;
  const zconfDst = `${src}/zconf.h`;
  if (existsSync(zconf) && !existsSync(zconfDst)) copyFileSync(zconf, zconfDst);
  const p = zlibPaths();
  if (!p) throw new Error("zlib build finished but library not found");
  return p.lib;
}

/** Build static brotli for unci brot + libheif oracle. */
export async function buildBrotli(): Promise<BrotliPaths> {
  await getDeps();
  await requireCmakeNinja();
  const src = `${ROOT}/deps/brotli`;
  const buildDir = `${ROOT}/out/brotli_build`;
  const existing = brotliPaths();
  if (existing) {
    console.log(`brotli already built: ${existing.dec}`);
    return existing;
  }
  mkdirSync(buildDir, { recursive: true });
  await runCmd(
    `cmake -G Ninja -S "${src}" -B "${buildDir}" ` +
      `-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBROTLI_BUILD_TOOLS=OFF`,
  );
  await runCmd(`ninja -C "${buildDir}"`);
  const p = brotliPaths();
  if (!p) throw new Error("brotli build finished but libraries not found");
  return p;
}

/** Build static libheif with libde265 + dav1d backends (no plugins). */
export async function buildLibHeif(): Promise<string> {
  await getDeps();
  await requireCmakeNinja();
  await buildDav1d();
  await buildLibDe265();
  await buildZlib();
  await buildBrotli();
  const buildDir = `${ROOT}/out/libheif_build`;
  const lib = `${buildDir}/libheif/heif.lib`;
  const libA = `${buildDir}/libheif/libheif.a`;
  /* Stamp: rebuild when unci / compression flags change. */
  const stamp = `${buildDir}/.heic_oracle_stamp`;
  const stampWant = "unci=1;de265=1;dav1d=1;zlib=1;brotli=1";
  const stampOk = existsSync(stamp) && readFileSync(stamp, "utf8").trim() === stampWant;
  if ((existsSync(lib) || existsSync(libA)) && stampOk) {
    const p = existsSync(lib) ? lib : libA;
    console.log(`libheif already built: ${p}`);
    return p;
  }
  if (existsSync(buildDir) && !stampOk) {
    console.log("libheif: reconfigure for unci + zlib/brotli...");
    /* Drop cache so FindZLIB/FindBrotli re-run with new paths. */
    const cache = `${buildDir}/CMakeCache.txt`;
    if (existsSync(cache)) rmSync(cache);
  }
  /* Upstream FindBrotli.cmake sets BROTLI_LIBS with a typo (BROTLICOMMON_LIBRARY). */
  const findBrotli = `${ROOT}/deps/libheif/cmake/modules/FindBrotli.cmake`;
  if (existsSync(findBrotli)) {
    const txt = readFileSync(findBrotli, "utf8");
    if (txt.includes("BROTLICOMMON_LIBRARY")) {
      writeFileSync(
        findBrotli,
        txt.replace(
          'set(BROTLI_LIBS "${BROTLICOMMON_LIBRARY}" "${BROTLI_DEC_LIB}"  "${BROTLI_ENC_LIB}")',
          'set(BROTLI_LIBS "${BROTLI_COMMON_LIB}" "${BROTLI_DEC_LIB}" "${BROTLI_ENC_LIB}")',
        ),
      );
    }
  }
  mkdirSync(buildDir, { recursive: true });
  const de265Inc = `${ROOT}/deps/libde265`;
  const de265Lib = existsSync(`${ROOT}/out/libde265_build/libde265/libde265.lib`)
    ? `${ROOT}/out/libde265_build/libde265/libde265.lib`
    : `${ROOT}/out/libde265_build/libde265/libde265.a`;
  const d = dav1dPaths();
  if (!d) throw new Error("dav1d missing after buildDav1d");
  const z = zlibPaths();
  const b = brotliPaths();
  if (!z || !b) throw new Error("zlib/brotli missing after build");
  /* LIBDE265_STATIC_BUILD: heif objects must not dllimport de265_* on Windows. */
  const staticDefs = isWindows
    ? `"/DLIBDE265_STATIC_BUILD /DLIBHEIF_STATIC_BUILD"`
    : `"-DLIBDE265_STATIC_BUILD -DLIBHEIF_STATIC_BUILD"`;
  const zlibInc = `${ROOT}/deps/zlib`;
  await runCmd(
    `cmake -G Ninja -S "${ROOT}/deps/libheif" -B "${buildDir}" ` +
      `-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF ` +
      `-DENABLE_PLUGIN_LOADING=OFF ` +
      `-DWITH_LIBDE265=ON -DWITH_LIBDE265_PLUGIN=OFF ` +
      `-DWITH_DAV1D=ON -DWITH_DAV1D_PLUGIN=OFF ` +
      `-DWITH_UNCOMPRESSED_CODEC=ON ` +
      `-DWITH_AOM_DECODER=OFF -DWITH_AOM_ENCODER=OFF ` +
      `-DWITH_X265=OFF -DWITH_X264=OFF -DWITH_SvtEnc=OFF -DWITH_RAV1E=OFF ` +
      `-DWITH_OpenH264_DECODER=OFF -DWITH_JPEG_DECODER=OFF -DWITH_JPEG_ENCODER=OFF ` +
      `-DWITH_OpenJPEG_DECODER=OFF -DWITH_OpenJPEG_ENCODER=OFF ` +
      `-DWITH_LIBSHARPYUV=OFF -DWITH_EXAMPLES=OFF -DWITH_GDK_PIXBUF=OFF ` +
      `-DBUILD_DOCUMENTATION=OFF -DBUILD_TESTING=OFF ` +
      `-DLIBDE265_INCLUDE_DIR="${de265Inc}" -DLIBDE265_LIBRARY="${de265Lib}" ` +
      `-DDAV1D_INCLUDE_DIR="${d.inc[d.inc.length - 1]}" -DDAV1D_LIBRARY="${d.lib}" ` +
      `-DZLIB_INCLUDE_DIR="${zlibInc}" -DZLIB_LIBRARY="${z.lib}" ` +
      `-DBROTLI_DEC_INCLUDE_DIR="${ROOT}/deps/brotli/c/include" ` +
      `-DBROTLI_ENC_INCLUDE_DIR="${ROOT}/deps/brotli/c/include" ` +
      `-DBROTLI_COMMON_LIB="${b.common}" -DBROTLI_DEC_LIB="${b.dec}" ` +
      `-DBROTLI_ENC_LIB="${b.enc}" ` +
      `-DCMAKE_CXX_FLAGS=${staticDefs} -DCMAKE_C_FLAGS=${staticDefs}`,
  );
  await runCmd(`ninja -C "${buildDir}"`);
  writeFileSync(stamp, stampWant);
  if (existsSync(lib)) return lib;
  if (existsSync(libA)) return libA;
  throw new Error("libheif build finished but heif.lib not found");
}

/** Build oracle stack (dav1d + libde265 + zlib + brotli + libheif). */
export async function buildRef(): Promise<void> {
  await buildDav1d();
  await buildLibDe265();
  await buildZlib();
  await buildBrotli();
  await buildLibHeif();
  console.log("ref: libheif oracle ready");
}

/* ----- libFuzzer + ASan target (driven by cmd/fuzz.ts) ----- */

// The exe links the DYNAMIC asan runtime (clang_rt.asan_dynamic-x86_64.dll),
// which lives in clang's resource dir, not on PATH; without a copy next to
// the exe the loader fails before main (STATUS_DLL_NOT_FOUND).
export async function copyAsanRuntimeDll(dir: string): Promise<void> {
  if (!isWindows) return;
  const dllName = "clang_rt.asan_dynamic-x86_64.dll";
  // Normalize: CopyFileW fails ENOENT on a path with a mixed-slash ".."
  // segment even when it resolves.
  const dst = resolvePath(dir, dllName);
  if (existsSync(dst)) return;
  const proc = Bun.spawnSync(["clang", "-print-resource-dir"]);
  const resDir = proc.stdout.toString().trim();
  const src = resolvePath(resDir, "lib/windows", dllName);
  if (!existsSync(src)) {
    console.warn(`warning: ${src} not found; ${dllName} must be on PATH`);
    return;
  }
  copyFileSync(src, dst);
}

/** Prefer Homebrew LLVM on macOS (Apple clang has no libFuzzer). */
function findFuzzClang(): string {
  const env = process.env.CLANG || process.env.CC || process.env.FUZZ_CC;
  if (env && existsSync(env)) return env;
  if (isMac) {
    for (const c of [
      "/opt/homebrew/opt/llvm/bin/clang",
      "/usr/local/opt/llvm/bin/clang",
    ]) {
      if (existsSync(c)) return c;
    }
  }
  return "clang";
}

const FUZZ_DIR = `${OUT_ROOT}/fuzz`;

/* ----- fuzz targets (shared by libFuzzer and AFL++) ----- */

export type FuzzTarget = "heic" | "container" | "hevc" | "diff";

export const FUZZ_TARGETS: Record<
  FuzzTarget,
  {
    /** file in test/ with LLVMFuzzerTestOneInput */
    src: string;
    /** link test/fuzz_mutator.c (ISOBMFF-aware; libFuzzer only) */
    mutator: boolean;
    /** needs the libheif oracle (Linux / macOS) */
    libheif: boolean;
    desc: string;
  }
> = {
  heic: {
    src: "fuzz_target.c",
    mutator: true,
    libheif: false,
    desc: "whole file: open, decode, metadata",
  },
  container: {
    src: "fuzz_container.c",
    mutator: true,
    libheif: false,
    desc: "ISOBMFF/HEIF parsing only, no pixel decode",
  },
  hevc: {
    src: "fuzz_hevc.c",
    mutator: false,
    libheif: false,
    desc: "raw Annex B HEVC stream, no container",
  },
  diff: {
    src: "fuzz_diff.c",
    mutator: true,
    libheif: true,
    desc: "decoded pixels compared with libheif",
  },
};

export function isFuzzTarget(name: string): name is FuzzTarget {
  return Object.hasOwn(FUZZ_TARGETS, name);
}

export type FuzzBuildOpts = {
  target?: FuzzTarget;
  /** asan (default) or msan (Linux only; pure C, no dav1d/zlib/brotli) */
  sanitizer?: "asan" | "msan";
  /** add UBSan, findings are fatal (not on Windows) */
  ubsan?: boolean;
  /** source-based coverage build instead of a sanitizer build */
  coverage?: boolean;
  /** leave dav1d / zlib / brotli out even when they are built */
  noDeps?: boolean;
  clean?: boolean;
};

function fuzzVariant(opts: FuzzBuildOpts): string {
  if (opts.coverage) return "cov";
  const base = opts.sanitizer === "msan" ? "msan" : "asan";
  return opts.ubsan ? `${base}-ubsan` : base;
}

/** Where buildFuzz puts the executable for these options. */
export function fuzzExePath(opts: FuzzBuildOpts = {}): string {
  const target = opts.target ?? "heic";
  return `${FUZZ_DIR}/${fuzzVariant(opts)}/${binName(`heic_fuzz_${target}`)}`;
}

/** llvm-profdata / llvm-cov / llvm-symbolizer that match the fuzz clang. */
export function findLlvmTool(name: string): string | null {
  const cc = findFuzzClang();
  const dir = cc.includes("/") ? cc.slice(0, cc.lastIndexOf("/")) : "";
  const version = /-(\d+)$/.exec(cc)?.[1];
  const names = version ? [`${name}-${version}`, name] : [name];
  for (const n of names) {
    if (dir && existsSync(`${dir}/${n}`)) return `${dir}/${n}`;
    const found = Bun.which(n);
    if (found) return found;
  }
  /* Debian/Ubuntu: only versioned names (llvm-cov-18) may be installed. */
  for (let v = 30; v >= 12; v--) {
    const found = Bun.which(`${name}-${v}`);
    if (found) return found;
  }
  return null;
}

// libFuzzer build of one fuzz target: SRCS + test/<target>.c (+ the custom
// mutator) with a sanitizer and -fsanitize=fuzzer (-O1 for readable traces).
// No main() \u2014 libFuzzer provides it. Each variant (asan, asan-ubsan, msan,
// msan-ubsan, cov) has its own directory under out/fuzz; the library objects
// in it are shared by the targets. Links dav1d/zlib/brotli when present so
// AVIF + unci compression paths are under coverage.
export async function buildFuzz(opts: FuzzBuildOpts = {}): Promise<string> {
  const target = opts.target ?? "heic";
  const info = FUZZ_TARGETS[target];
  const msan = !opts.coverage && opts.sanitizer === "msan";
  const variant = fuzzVariant(opts);
  const dir = `${FUZZ_DIR}/${variant}`;
  const exe = fuzzExePath(opts);

  if (msan && process.platform !== "linux")
    throw new Error("MemorySanitizer fuzzing needs Linux (clang -fsanitize=memory)");
  if (opts.ubsan && isWindows)
    throw new Error(
      "UBSan fuzzing is not supported on Windows (ASan+UBSan with the prebuilt /MD deps is flaky)",
    );
  if (info.libheif && isWindows)
    throw new Error(`fuzz target ${target} is Linux / macOS only`);
  if (info.libheif && msan)
    throw new Error(`fuzz target ${target} links libheif, which MSan cannot check`);

  /* MSan reports every value that passed through uninstrumented code, so the
     prebuilt dav1d / zlib / brotli cannot be linked into that build. */
  const noDeps = !!opts.noDeps || msan;
  const heif = info.libheif ? libheifPaths() : null;
  if (info.libheif && !heif)
    throw new Error(
      "libheif oracle not built. Run: bun cmd/build.ts -build-libheif",
    );
  const dav1d = noDeps && !heif ? null : dav1dPaths();
  const zlib = noDeps ? null : zlibPaths();
  const brotli = noDeps ? null : brotliPaths();
  const comp = compressFeatureFlags(zlib, brotli);
  let def = comp.def;
  let inc = ` -I${ROOT}/src -I${ROOT}/test` + comp.inc;
  if (dav1d) {
    def += " -DHEIC_HAVE_DAV1D";
    inc += dav1dIncFlags(dav1d);
  }
  if (heif) {
    def += " -DLIBHEIF_STATIC_BUILD -DLIBDE265_STATIC_BUILD";
    for (const d of heif.heifInc) inc += ` -I${d}`;
  }

  mkdirSync(dir, { recursive: true });
  /* Windows: MSVC /MD deps need __imp__aligned_malloc; see fuzz_win_md_shim.c. */
  const units: { src: string; obj: string }[] = [
    ...SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${dir}/${objBase(s)}.o`,
    })),
    {
      src: `${ROOT}/test/${info.src}`,
      obj: `${dir}/${objBase(info.src)}.o`,
    },
    ...(info.mutator
      ? [{ src: `${ROOT}/test/fuzz_mutator.c`, obj: `${dir}/fuzz_mutator.o` }]
      : []),
    ...(isWindows
      ? [
          {
            src: `${ROOT}/test/fuzz_win_md_shim.c`,
            obj: `${dir}/fuzz_win_md_shim.o`,
          },
        ]
      : []),
  ];

  const cc = findFuzzClang();
  const sanitize = opts.coverage
    ? "fuzzer"
    : [msan ? "memory" : "address", ...(opts.ubsan ? ["undefined"] : []), "fuzzer"].join(",");
  const extra = opts.coverage
    ? " -fprofile-instr-generate -fcoverage-mapping"
    : (msan ? " -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer" : "") +
      (opts.ubsan ? " -fno-sanitize-recover=undefined" : "");
  // -O1 for readable sanitizer traces (same as djvudec fuzz).
  const cflags = `-fsanitize=${sanitize}${extra} ${clangCFlags("-g -O1")}${def}${inc}`;

  /* The library objects are shared by the targets of a variant: rebuild
     them all when the flags they were compiled with change. */
  const stampPath = `${dir}/.flags`;
  const stampWant = `${cc} ${cflags}`;
  const stampChanged =
    !existsSync(stampPath) || readFileSync(stampPath, "utf8") !== stampWant;
  if (opts.clean || stampChanged) {
    for (const u of units) rmSync(u.obj, { force: true });
    rmSync(exe, { force: true });
  }

  const headers = [...trackedHeaders(), `${ROOT}/test/fuzz_common.h`];
  const objStale = (u: { src: string; obj: string }) =>
    needsRebuild(u.obj, u.src, ...headers);
  /* libheif brings its own needs: dav1d, and the brotli encoder / zlib it
     was configured with, whether or not our side uses them. */
  const heifBrotli = heif ? brotliPaths() : null;
  const heifZlib = heif ? zlibPaths() : null;
  const linkInputs = heif
    ? [
        heif.heifLib,
        heif.de265Lib,
        heif.dav1dLib,
        ...(heifBrotli ? [heifBrotli.enc, heifBrotli.dec, heifBrotli.common] : []),
        ...(heifZlib ? [heifZlib.lib] : []),
      ]
    : [...(dav1d ? [dav1d.lib] : []), ...comp.libs];
  const stale = units.filter(objStale);
  const name = `heic_fuzz_${target}`;
  if (
    !stale.length && existsSync(exe)
    && !needsRebuild(exe, ...units.map((u) => u.obj), ...linkInputs)
  ) {
    if (isWindows) await copyAsanRuntimeDll(dir);
    console.log(`${name} (${variant}) up to date`);
    return exe;
  }

  console.log(
    `building ${name} (clang+${sanitize}` +
      `${opts.coverage ? "+coverage" : ""}` +
      `${heif ? "+libheif" : ""}${dav1d ? "+dav1d" : ""}` +
      `${zlib ? "+zlib" : ""}${brotli ? "+brotli" : ""})...`,
  );
  try {
    await runPool(stale, 8, async (u) => {
      await $`${cc} ${{ raw: cflags }} -c -o ${u.obj} ${u.src}`.cwd(ROOT).quiet();
    });
    writeFileSync(stampPath, stampWant);
    const linkLibs: string[] = [...linkInputs];
    if (!isWindows) linkLibs.push("-lpthread", "-lm");
    /* libheif and libde265 are C++. */
    if (heif) linkLibs.push(isMac ? "-lc++" : "-lstdc++");
    const linkExtra = linkLibs.length ? ` ${linkLibs.join(" ")}` : "";
    /* ASan inflates stack frames; Windows default 1MB is tight for nested
     * HEVC TT/CQT + residual under fuzz REDUCE. Give the fuzzer 8MB. */
    const stackFlag = isWindows ? " -Wl,/STACK:8388608" : "";
    const linkSan = `-fsanitize=${sanitize}${opts.coverage ? " -fprofile-instr-generate" : ""}`;
    const objs = units.map((u) => u.obj).join(" ");
    await $`${cc} ${{ raw: linkSan }} ${{ raw: objs }}${{ raw: linkExtra }}${{ raw: stackFlag }} -o ${exe}`.cwd(
      ROOT,
    );
  } catch (e) {
    if (isMac && cc === "clang") {
      console.error("\nfuzz build failed: Apple's clang does not include libFuzzer.");
      console.error("Install LLVM from Homebrew which provides a clang with fuzzer support:");
      console.error("  brew install llvm");
      console.error("Then re-run. The script will auto-detect /opt/homebrew/opt/llvm/bin/clang.\n");
    }
    throw e;
  }
  if (isWindows) await copyAsanRuntimeDll(dir);
  console.log(`built ${name} (${variant})`);
  return exe;
}

/* ----- AFL++ target (driven by cmd/fuzz-afl.ts; macOS / *nix) ----- */

const FUZZ_AFL_DIR = `${OUT_ROOT}/fuzz-afl`;
/** Where buildAflFuzz puts the executable for these options. */
export function aflExePath(
  opts: { target?: FuzzTarget; asan?: boolean; cmplog?: boolean } = {},
): string {
  const target = opts.target ?? "heic";
  const variant = opts.cmplog ? "cmplog" : opts.asan !== false ? "asan" : "plain";
  return `${FUZZ_AFL_DIR}/${variant}/heic_afl_${target}`;
}

/** Prefer Homebrew AFL++ wrappers (afl-clang-fast → Homebrew LLVM). */
export function findAflCc(): string {
  const env = process.env.AFL_CC || process.env.AFL_CLANG_FAST;
  if (env && existsSync(env)) return env;
  for (const c of [
    "/opt/homebrew/bin/afl-clang-fast",
    "/usr/local/bin/afl-clang-fast",
    "/opt/homebrew/bin/afl-cc",
    "/usr/local/bin/afl-cc",
  ]) {
    if (existsSync(c)) return c;
  }
  return "afl-clang-fast";
}

export function aflTool(name: string): string | null {
  const envKey = name.toUpperCase().replaceAll("-", "_");
  const env = process.env[envKey];
  if (env && existsSync(env)) return env;
  for (const base of ["/opt/homebrew/bin", "/usr/local/bin"]) {
    const p = `${base}/${name}`;
    if (existsSync(p)) return p;
  }
  // Bun.which returns null when missing.
  return Bun.which(name);
}

// AFL++ target: same LLVMFuzzerTestOneInput harness as libFuzzer, but built
// with afl-clang-fast -fsanitize=fuzzer (AFL++ swaps that for libAFLDriver.a
// + persistent-mode shared-memory input). Optional ASan via AFL_USE_ASAN.
// dav1d/zlib/brotli are linked uninstrumented (prebuilt) when present.
// cmplog: the second binary afl-fuzz -c wants (AFL_LLVM_CMPLOG=1, no ASan);
// it logs comparison operands so AFL++ can solve magic values and lengths.
export async function buildAflFuzz(
  opts: {
    target?: FuzzTarget;
    clean?: boolean;
    asan?: boolean;
    cmplog?: boolean;
  } = {},
): Promise<string> {
  if (isWindows) {
    throw new Error(
      "AFL++ fuzzing is not supported on Windows; use bun cmd/fuzz.ts (libFuzzer)",
    );
  }
  const clean = !!opts.clean;
  const cmplog = !!opts.cmplog;
  const asan = !cmplog && opts.asan !== false; // default on
  const target = opts.target ?? "heic";
  const info = FUZZ_TARGETS[target];
  if (info.libheif)
    throw new Error(`fuzz target ${target} is not built for AFL++ (use bun cmd/fuzz.ts)`);
  const aflExe = aflExePath({ target, asan, cmplog });
  const aflDir = aflExe.slice(0, aflExe.lastIndexOf("/"));
  mkdirSync(aflDir, { recursive: true });

  const cc = findAflCc();
  const ccOk =
    existsSync(cc) ||
    !!Bun.which(cc) ||
    // bare name may still resolve when PATH is set for the child
    cc === "afl-clang-fast" ||
    cc === "afl-cc";
  if (!ccOk && !Bun.which("afl-clang-fast") && !Bun.which("afl-cc")) {
    throw new Error(
      "afl-clang-fast not found. macOS: brew install afl++; Debian/Ubuntu: sudo apt install afl++\n" +
        "  (uses Homebrew LLVM for instrumentation)",
    );
  }

  const units: { src: string; obj: string }[] = [
    ...SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${aflDir}/${objBase(s)}.o`,
    })),
    {
      src: `${ROOT}/test/${info.src}`,
      obj: `${aflDir}/${objBase(info.src)}.o`,
    },
  ];

  const dav1d = dav1dPaths();
  const zlib = zlibPaths();
  const brotli = brotliPaths();
  const comp = compressFeatureFlags(zlib, brotli);
  const stampWant =
    `asan=${asan ? 1 : 0};cmplog=${cmplog ? 1 : 0};dav1d=${dav1d ? 1 : 0};zlib=${zlib ? 1 : 0};brotli=${brotli ? 1 : 0};cc=${cc}`;
  const stampPath = `${aflDir}/.afl_features`;
  const stampPrev = existsSync(stampPath)
    ? readFileSync(stampPath, "utf8").trim()
    : "";
  const stampChanged = stampPrev !== stampWant;

  if (clean || stampChanged) {
    for (const u of units) rmSync(u.obj, { force: true });
    rmSync(aflExe, { force: true });
  }

  let def = comp.def;
  let inc = ` -I${ROOT}/src -I${ROOT}/test` + comp.inc;
  if (dav1d) {
    def += " -DHEIC_HAVE_DAV1D";
    inc += dav1dIncFlags(dav1d);
  }

  const headers = [...trackedHeaders(), `${ROOT}/test/fuzz_common.h`];
  const objStale = (u: { src: string; obj: string }) =>
    needsRebuild(u.obj, u.src, ...headers);
  const staleObj = units.some(objStale);
  const staleExe = needsRebuild(
    aflExe,
    ...units.map((u) => u.obj),
    ...(dav1d ? [dav1d.lib] : []),
    ...comp.libs,
  );
  if (!staleObj && !staleExe && existsSync(aflExe) && !stampChanged) {
    console.log(`heic_afl_${target} up to date`);
    return aflExe;
  }

  // -O1 for readable ASan traces; -fsanitize=fuzzer → AFL++ libAFLDriver.
  // Do not also pass -fsanitize=address: AFL_USE_ASAN adds it.
  const cflags = `-fsanitize=fuzzer ${clangCFlags("-g -O1", false)}${def}${inc}`;
  const env: Record<string, string> = {
    ...process.env,
    // Strip -Werror noise from AFL plugin passes on some LLVM versions.
    AFL_LLVM_NO_ERROR: process.env.AFL_LLVM_NO_ERROR ?? "1",
    AFL_QUIET: process.env.AFL_QUIET ?? "1",
  };
  if (asan) env.AFL_USE_ASAN = "1";
  else delete env.AFL_USE_ASAN;
  if (cmplog) env.AFL_LLVM_CMPLOG = "1";
  else delete env.AFL_LLVM_CMPLOG;

  console.log(
    `building heic_afl_${target} (afl-clang-fast+fuzzer` +
      `${asan ? "+asan" : ""}${cmplog ? "+cmplog" : ""}` +
      `${dav1d ? "+dav1d" : ""}${zlib ? "+zlib" : ""}${brotli ? "+brotli" : ""})...`,
  );
  try {
    for (const u of units) {
      if (!objStale(u) && !stampChanged) continue;
      await $`${cc} ${{ raw: cflags }} -c -o ${u.obj} ${u.src}`
        .cwd(ROOT)
        .env(env);
    }
    const objs = units.map((u) => u.obj);
    const linkLibs: string[] = [];
    if (dav1d) linkLibs.push(dav1d.lib);
    linkLibs.push(...comp.libs);
    linkLibs.push("-lpthread", "-lm");
    const linkExtra = ` ${linkLibs.join(" ")}`;
    if (
      needsRebuild(
        aflExe,
        ...objs,
        ...(dav1d ? [dav1d.lib] : []),
        ...comp.libs,
      ) ||
      stampChanged
    ) {
      await $`${cc} -fsanitize=fuzzer ${{ raw: objs.join(" ") }}${{ raw: linkExtra }} -o ${aflExe}`
        .cwd(ROOT)
        .env(env);
    }
  } catch (e) {
    console.error("\nAFL++ fuzz build failed.");
    console.error("Install AFL++ (and LLVM):");
    console.error("  macOS:          brew install afl++");
    console.error("  Debian/Ubuntu:  sudo apt install afl++");
    console.error(
      "afl-clang-fast must be on PATH (or set AFL_CC=/path/to/afl-clang-fast).\n",
    );
    throw e;
  }
  writeFileSync(stampPath, stampWant);
  console.log("built heic_afl");
  return aflExe;
}

if (import.meta.main) {
  const argv = process.argv.slice(2);
  if (argv.includes("-clean")) cleanBuildOutput();
  if (argv.includes("ref") || argv.includes("-build-libheif")) {
    await buildRef();
    if (!argv.includes("-libheif") && !argv.includes("-clang") && !argv.includes("-msvc") && !argv.includes("-dav1d")) {
      process.exit(0);
    }
  }
  if (argv.includes("-build-dav1d")) {
    const lib = await buildDav1d();
    console.log(`built dav1d: ${lib}`);
    if (!argv.includes("-dav1d") && !argv.includes("-libheif") && !argv.includes("-clang") && !argv.includes("-msvc")) {
      process.exit(0);
    }
  }
  const clang =
    argv.includes("-clang") ? true : argv.includes("-msvc") ? false : defaultUseClang;
  const withLibheif = argv.includes("-libheif");
  const withDav1d = argv.includes("-dav1d") || withLibheif;
  if (withDav1d && !dav1dPaths()) await buildDav1d();
  /* unci compression deps (used by harness and by libheif oracle) */
  if (!zlibPaths()) await buildZlib();
  if (!brotliPaths()) await buildBrotli();
  if (withLibheif && !libheifPaths()) await buildRef();
  else if (withLibheif) {
    /* rebuild libheif if stamp lacks zlib/brotli */
    const stamp = `${ROOT}/out/libheif_build/.heic_oracle_stamp`;
    const want = "unci=1;de265=1;dav1d=1;zlib=1;brotli=1";
    if (!existsSync(stamp) || readFileSync(stamp, "utf8").trim() !== want) {
      await buildLibHeif();
    }
  }
  const exe = await build(clang, { withDav1d, withLibheif });
  const tags = [
    withDav1d ? "dav1d" : null,
    withLibheif ? "libheif" : null,
    zlibPaths() ? "zlib" : null,
    brotliPaths() ? "brotli" : null,
  ]
    .filter(Boolean)
    .join("+");
  console.log(`built ${exe}${tags ? ` (${tags})` : ""}`);
}
