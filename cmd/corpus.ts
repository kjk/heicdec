// Corpus: HEIC/HEIF/AVIF under deps/heic/testdata, Nokia heif_conformance,
// libheif examples, deps/testimages (and optional HEIC_SPECS override).
import { existsSync, readdirSync, statSync } from "fs";
import { basename, isAbsolute, join, relative } from "path";
import { DEPS_DIR, TESTIMAGES_DIR } from "./get-deps";

const EXT = new Set([".heic", ".heif", ".avif", ".avifs"]);
const ROOT = join(import.meta.dir, "..");

/** Default roots when HEIC_SPECS is unset (order = preference for basename dedup). */
export function corpusRoots(): string[] {
  return [
    join(DEPS_DIR, "heic", "testdata"),
    join(DEPS_DIR, "libheif", "examples"),
    /* Nokia HEIF conformance candidates (C001.heic …); HEVC stills + grids/iovl/etc. */
    join(DEPS_DIR, "heif_conformance", "conformance_files"),
    /* Downloaded + generated fixtures (get-deps → deps/testimages). */
    join(TESTIMAGES_DIR, "avif"),
    join(TESTIMAGES_DIR, "unci_block"),
    /* ISO/IEC 14496-15 HEVC and AV1 Mini boxes copied from libheif's corpus. */
    join(TESTIMAGES_DIR, "mini"),
  ];
}

function walk(dir: string): string[] {
  if (!existsSync(dir)) return [];
  const out: string[] = [];
  for (const name of readdirSync(dir)) {
    /* Skip private source blobs used only by fixture generators. */
    if (name.startsWith("_") || name === "src") continue;
    const p = join(dir, name);
    let st;
    try {
      st = statSync(p);
    } catch {
      continue;
    }
    if (st.isDirectory()) out.push(...walk(p));
    else {
      const i = name.lastIndexOf(".");
      if (i >= 0 && EXT.has(name.slice(i).toLowerCase())) out.push(p);
    }
  }
  return out;
}

export function corpusFiles(): string[] {
  if (process.env.HEIC_SPECS) return walk(process.env.HEIC_SPECS).sort();
  const seen = new Set<string>();
  const out: string[] = [];
  for (const root of corpusRoots()) {
    for (const f of walk(root)) {
      const key = basename(f).toLowerCase();
      if (seen.has(key)) continue;
      seen.add(key);
      out.push(f);
    }
  }
  return out.sort();
}

export function pickRandom(files: string[], n: number): string[] {
  const shuffled = [...files];
  for (let i = shuffled.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
  }
  return shuffled.slice(0, Math.max(0, Math.min(n, shuffled.length)));
}

export function corpusSummary(): string {
  const files = corpusFiles();
  const nokia = files.filter((f) => /heif_conformance/i.test(f)).length;
  const base = files.length - nokia;
  if (nokia > 0)
    return `${files.length} corpus images (${base} imazen/libheif + ${nokia} Nokia conformance)`;
  return `${files.length} corpus images`;
}

/** Path relative to `root` when possible (forward slashes). */
export function fileRel(f: string, root: string): string {
  let rel = relative(root, f);
  if (rel.startsWith("..") || isAbsolute(rel)) rel = f;
  return rel.replaceAll("\\", "/");
}

/** Basename + size for compact bench lines under a directory header. */
export function fileNameLabel(f: string): string {
  const size = statSync(f).size;
  return `${basename(f)} : ${size.toLocaleString("en-US")} bytes`;
}

/** Full relative path + size (e.g. top-10 summary). */
export function fileLabel(f: string, root: string): string {
  const size = statSync(f).size;
  return `${fileRel(f, root)} : ${size.toLocaleString("en-US")} bytes`;
}

export function selectFiles(
  usageText: string,
  valueFlags: string[] = ["-rand"],
): string[] {
  const argv = process.argv.slice(2);
  const explicit = argv.filter(
    (a, i) => !a.startsWith("-") && !valueFlags.includes(argv[i - 1] ?? ""),
  );
  if (argv.includes("-all")) return corpusFiles();
  const ri = argv.indexOf("-rand");
  if (ri >= 0) {
    const n = parseInt(argv[ri + 1] ?? "0", 10);
    if (!n) {
      console.error(usageText);
      process.exit(2);
    }
    return pickRandom(corpusFiles(), n);
  }
  if (explicit.length) return explicit;
  console.error(usageText);
  process.exit(2);
}
