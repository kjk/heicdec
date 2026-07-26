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

const INTERNAL_H = `${ROOT}/src/heic_internal.h`;
const PUBLIC_H = `${ROOT}/src/heic.h`;

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
  return units;
}

const harnessExeName = (useClang: boolean) =>
  binName(`heic_test_${useClang ? "clang" : "msvc"}`);

export type BuildOpts = {
  useClang?: boolean;
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

/** When feature / cflags stamps change, force a full recompile of that out dir. */
function syncFeatureStamp(
  dir: string,
  useClang: boolean,
  stampName: string,
  want: string,
  objExt: string,
  _extraObjs: string[] = [],
): void {
  const stamp = `${dir}/${stampName}`;
  const prev = existsSync(stamp) ? readFileSync(stamp, "utf8").trim() : "";
  if (prev !== want) {
    /* Drop every object + the harness so -Zi/-g and feature defines stay coherent. */
    if (existsSync(dir)) {
      for (const name of readdirSync(dir)) {
        if (name.endsWith(`.${objExt}`) || name.endsWith(".pdb") || name.endsWith(".exe")) {
          rmSync(`${dir}/${name}`, { force: true });
        }
      }
    }
    const exe = `${dir}/${harnessExeName(useClang)}`;
    if (existsSync(exe)) rmSync(exe);
    mkdirSync(dir, { recursive: true });
    writeFileSync(stamp, want);
  }
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
  syncFeatureStamp(
    dir,
    true,
    ".heic_features",
    `dav1d=${needDav1d ? 1 : 0};libheif=${withLibheif ? 1 : 0};zlib=${zlib ? 1 : 0};brotli=${brotli ? 1 : 0};pdb=1`,
    "o",
  );
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
  const units = cUnits(dir, "o", withLibheif);
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    await runCmd(
      `clang ${clangCFlags()}${def}${inc} -c -o ${u.obj} ${u.src}`,
    );
  }
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
  syncFeatureStamp(
    dir,
    false,
    ".heic_features",
    `dav1d=${needDav1d ? 1 : 0};libheif=${withLibheif ? 1 : 0};zlib=${zlib ? 1 : 0};brotli=${brotli ? 1 : 0};pdb=1`,
    "obj",
  );
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
  const units = cUnits(dir, "obj", withLibheif);
  /* cmake builds heif/libde265 with /MD; match that when linking the oracle. */
  const msvcC = withLibheif
    ? HEIC_MSVC_CL_C.replace(/\s-MT\b/, " -MD")
    : HEIC_MSVC_CL_C;
  /* One PDB for all objs + the final exe (winperf symbolicates from this). */
  const pdb = `${dir}/heic_test_msvc.pdb`;
  const clC = `${msvcC}${def}${inc} -Fo${dir}/ -Fd${pdb} -c`;
  for (const u of units) {
    if (!needsRebuild(u.obj, u.src, INTERNAL_H, PUBLIC_H)) continue;
    const rel = u.src.startsWith(`${ROOT}/`)
      ? u.src.slice(ROOT.length + 1)
      : u.src;
    await runCmd(`cl ${clC} ${rel}`, ROOT);
  }
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
    const link = withLibheif
      ? `cl -nologo -MD -Zi ${objs.join(" ")}${linkExtra} -Fe:${exePath} -Fd${pdb} -link -DEBUG -DEFAULTLIB:msvcprt`
      : `cl -nologo -Zi ${objs.join(" ")}${linkExtra} -Fe:${exePath} -Fd${pdb} -link -DEBUG`;
    await runCmd(link, ROOT);
  }
  return exePath;
}

export async function build(
  useClang: boolean = defaultUseClang,
  opts: BuildOpts = {},
): Promise<string> {
  await getDeps();
  const o = { ...opts, useClang };
  if (useClang) return buildClang(o);
  return buildMsvc(o);
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
export const FUZZ_EXE = `${FUZZ_DIR}/${isWindows ? "heic_fuzz.exe" : "heic_fuzz"}`;

// libFuzzer target: SRCS + test/fuzz_target.c, ASan + fuzzer (-O1 for readable
// traces). No main() — libFuzzer provides it. Links dav1d/zlib/brotli when
// present so AVIF + unci compression paths are under coverage.
export async function buildFuzz(clean = false): Promise<string> {
  mkdirSync(FUZZ_DIR, { recursive: true });
  const testSrc = `${ROOT}/test/fuzz_target.c`;
  /* Windows: MSVC /MD deps need __imp__aligned_malloc; see fuzz_win_md_shim.c. */
  const winShimSrc = `${ROOT}/test/fuzz_win_md_shim.c`;
  const units: { src: string; obj: string }[] = [
    ...SRCS.map((s) => ({
      src: `${ROOT}/${s}`,
      obj: `${FUZZ_DIR}/${objBase(s)}.o`,
    })),
    {
      src: testSrc,
      obj: `${FUZZ_DIR}/fuzz_target.o`,
    },
    ...(isWindows
      ? [{ src: winShimSrc, obj: `${FUZZ_DIR}/fuzz_win_md_shim.o` }]
      : []),
  ];
  if (clean) {
    for (const u of units) rmSync(u.obj, { force: true });
    rmSync(FUZZ_EXE, { force: true });
  }

  const dav1d = dav1dPaths();
  const zlib = zlibPaths();
  const brotli = brotliPaths();
  const comp = compressFeatureFlags(zlib, brotli);
  let def = comp.def;
  let inc = ` -I${ROOT}/src` + comp.inc;
  if (dav1d) {
    def += " -DHEIC_HAVE_DAV1D";
    inc += dav1dIncFlags(dav1d);
  }

  const headers = [INTERNAL_H, PUBLIC_H];
  const objStale = (u: { src: string; obj: string }) =>
    needsRebuild(u.obj, u.src, ...headers);
  const staleObj = units.some(objStale);
  const staleExe = needsRebuild(
    FUZZ_EXE,
    ...units.map((u) => u.obj),
    ...(dav1d ? [dav1d.lib] : []),
    ...comp.libs,
  );
  if (!staleObj && !staleExe && existsSync(FUZZ_EXE)) {
    if (isWindows) await copyAsanRuntimeDll(FUZZ_DIR);
    console.log("heic_fuzz up to date");
    return FUZZ_EXE;
  }

  const cc = findFuzzClang();
  // -O1 for readable ASan traces (same as djvudec fuzz).
  const cflags = `-fsanitize=address,fuzzer ${clangCFlags("-g -O1")}${def}${inc}`;
  console.log(
    `building heic_fuzz (clang+asan+fuzzer` +
      `${dav1d ? "+dav1d" : ""}${zlib ? "+zlib" : ""}${brotli ? "+brotli" : ""})...`,
  );
  try {
    for (const u of units) {
      if (!objStale(u)) continue;
      await $`${cc} ${{ raw: cflags }} -c -o ${u.obj} ${u.src}`.cwd(ROOT);
    }
    const objs = units.map((u) => u.obj);
    const linkLibs: string[] = [];
    if (dav1d) linkLibs.push(dav1d.lib);
    linkLibs.push(...comp.libs);
    if (!isWindows) linkLibs.push("-lpthread", "-lm");
    const linkExtra = linkLibs.length ? ` ${linkLibs.join(" ")}` : "";
    if (needsRebuild(FUZZ_EXE, ...objs, ...(dav1d ? [dav1d.lib] : []), ...comp.libs)) {
      /* ASan inflates stack frames; Windows default 1MB is tight for nested
       * HEVC TT/CQT + residual under fuzz REDUCE. Give the fuzzer 8MB. */
      const stackFlag = isWindows ? " -Wl,/STACK:8388608" : "";
      await $`${cc} -fsanitize=address,fuzzer ${{ raw: objs.join(" ") }}${{ raw: linkExtra }}${{ raw: stackFlag }} -o ${FUZZ_EXE}`.cwd(
        ROOT,
      );
    }
  } catch (e) {
    if (isMac && cc === "clang") {
      console.error("\nfuzz build failed: Apple's clang does not include libFuzzer.");
      console.error("Install LLVM from Homebrew which provides a clang with fuzzer support:");
      console.error("  brew install llvm");
      console.error("Then re-run. The script will auto-detect /opt/homebrew/opt/llvm/bin/clang.\n");
    }
    throw e;
  }
  if (isWindows) await copyAsanRuntimeDll(FUZZ_DIR);
  console.log("built heic_fuzz");
  return FUZZ_EXE;
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
