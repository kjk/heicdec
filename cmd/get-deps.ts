// get-deps.ts -- fetch reference checkouts (corpus + oracles) and local fixtures.
//
//   bun cmd/get-deps.ts
//
// Clones into deps/ (skipped if present). deps/ is gitignored.
// Also fills deps/testimages/ with AVIF samples, official HEIF Mini samples,
// and regenerated grid/alpha + unci block fixtures (no checked-in binaries).
import { $ } from "bun";
import { copyFileSync, existsSync, mkdirSync, writeFileSync } from "fs";
import { join } from "path";
import { generateAvifFixtures } from "./gen_avif_fixtures";
import { generateDependentSliceHeif } from "./gen_hevc_fixtures";
import { generateUnciBlockFixtures } from "./gen_unci_block_fixtures";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
export const DEPS_DIR = join(ROOT, "deps");

export const HEIC_RUST_DIR = join(DEPS_DIR, "heic");
export const DAV1D_DIR = join(DEPS_DIR, "dav1d");
export const LIBHEIF_DIR = join(DEPS_DIR, "libheif");
export const LIBDE265_DIR = join(DEPS_DIR, "libde265");
export const ZLIB_DIR = join(DEPS_DIR, "zlib");
export const BROTLI_DIR = join(DEPS_DIR, "brotli");
export const HEIF_CONFORMANCE_DIR = join(DEPS_DIR, "heif_conformance");
/** Downloaded + generated test images (AVIF, HEIF Mini, unci block). */
export const TESTIMAGES_DIR = join(DEPS_DIR, "testimages");

const REPOS = [
  // Rust source being ported + primary test corpus (testdata/)
  { url: "https://github.com/imazen/heic", dir: "heic" },
  // AV1 decoder we link (C). Not re-ported.
  { url: "https://code.videolan.org/videolan/dav1d.git", dir: "dav1d" },
  // Pixel/oracle + API inspiration (needs libde265 + dav1d)
  { url: "https://github.com/strukturag/libheif", dir: "libheif" },
  // HEVC decoder plugin for libheif oracle
  { url: "https://github.com/strukturag/libde265", dir: "libde265" },
  // unci zlib/deflate (ISO 23001-17 generic compression)
  { url: "https://github.com/madler/zlib.git", dir: "zlib" },
  // unci brotli
  { url: "https://github.com/google/brotli.git", dir: "brotli" },
  // Nokia HEIF conformance candidates (C001.heic …) — HEVC-era HEIF
  {
    url: "https://github.com/nokiatech/heif_conformance.git",
    dir: "heif_conformance",
  },
];

const FOX_BASE =
  "https://raw.githubusercontent.com/link-u/avif-sample-images/master";
const FOX_SAMPLES = [
  "fox.profile0.8bpc.yuv420.avif",
  "fox.profile0.10bpc.yuv420.avif",
  "fox.profile0.8bpc.yuv420.monochrome.avif",
];
const LIBAVIF_BASE =
  "https://raw.githubusercontent.com/AOMediaCodec/libavif/" +
  "57953b32c1d5ae09761ce8ef66141cfa78c6196f/tests/data";
const LIBAVIF_SAMPLES = [
  "colors-animated-8bpc.avif",
  "colors-animated-8bpc-alpha-exif-xmp.avif",
];

const STAMP = ".heic_testimages_stamp";
const STAMP_WANT =
  "v10-avif-fox+grid+alpha+meta-moov-sequence+sequence-alpha;unci-block;mini-hevc+av1;hevc-sequences+pcm+dependent-slices+wpp";
const HEVC_SEQUENCE_BASE = "https://fate-suite.ffmpeg.org/hevc-conformance";
const HEVC_SEQUENCE_SAMPLES = [
  "LTRPSPS_A_Qualcomm_1.bit",
  "RPLM_A_qualcomm_4.bit",
  "TMVP_A_MS_3.bit",
  "WP_A_Toshiba_3.bit",
  "WP_B_Toshiba_3.bit",
  "MERGE_A_TI_3.bit",
  "DSLICE_A_HHI_5.bit",
  "DSLICE_B_HHI_5.bit",
  "ipcm_A_NEC_3.bit",
  "ipcm_B_NEC_3.bit",
  "ipcm_C_NEC_3.bit",
];

async function download(url: string, dest: string): Promise<void> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`GET ${url} → ${res.status}`);
  const buf = new Uint8Array(await res.arrayBuffer());
  writeFileSync(dest, buf);
  console.log(`deps/testimages: downloaded ${dest} (${buf.length} bytes)`);
}

/** Download link-u fox AVIFs and regenerate grid/alpha + unci block fixtures. */
export async function ensureTestImages(opts: { force?: boolean } = {}): Promise<void> {
  const root = TESTIMAGES_DIR;
  const avifDir = join(root, "avif");
  const unciDir = join(root, "unci_block");
  const miniDir = join(root, "mini");
  const hevcSequenceDir = join(root, "hevc_sequence");
  const stampPath = join(root, STAMP);
  if (
    !opts.force &&
    existsSync(stampPath) &&
    existsSync(join(avifDir, "grid_2x2.avif")) &&
    existsSync(join(avifDir, "alpha.avif")) &&
    existsSync(join(avifDir, "sequence_inter.avif")) &&
    existsSync(join(avifDir, "sequence_alpha.avif")) &&
    existsSync(join(unciDir, "rgb8_block_pixel_le.heif")) &&
    existsSync(join(miniDir, "hevc32-mini.heif")) &&
    existsSync(join(miniDir, "avif32-mini.heif")) &&
    existsSync(join(miniDir, "dependent-slices.heic")) &&
    existsSync(join(miniDir, "dependent-slices-wpp.heic")) &&
    HEVC_SEQUENCE_SAMPLES.every((name) =>
      existsSync(join(hevcSequenceDir, name)),
    )
  ) {
    const prev = (await Bun.file(stampPath).text()).trim();
    if (prev === STAMP_WANT) {
      console.log(`deps/testimages: up to date (${STAMP_WANT})`);
      return;
    }
  }

  mkdirSync(join(avifDir, "_src"), { recursive: true });
  mkdirSync(unciDir, { recursive: true });
  mkdirSync(miniDir, { recursive: true });
  mkdirSync(hevcSequenceDir, { recursive: true });

  for (const name of FOX_SAMPLES) {
    const dest =
      name.includes("monochrome")
        ? join(avifDir, "_src", name)
        : join(avifDir, name);
    if (!existsSync(dest) || opts.force) {
      console.log(`deps/testimages: fetching ${name}…`);
      await download(`${FOX_BASE}/${name}`, dest);
    }
  }
  for (const name of LIBAVIF_SAMPLES) {
    const dest = join(avifDir, "_src", name);
    if (!existsSync(dest) || opts.force) {
      console.log(`deps/testimages: fetching ${name}…`);
      await download(`${LIBAVIF_BASE}/${name}`, dest);
    }
  }

  console.log("deps/testimages: generating grid/alpha AVIFs…");
  generateAvifFixtures(avifDir);
  console.log("deps/testimages: generating unci block HEIFs…");
  generateUnciBlockFixtures(unciDir);
  console.log("deps/testimages: installing HEVC/AV1 Mini fixtures…");
  copyFileSync(
    join(LIBHEIF_DIR, "fuzzing", "data", "corpus", "hevc32-mini.heif"),
    join(miniDir, "hevc32-mini.heif"),
  );
  copyFileSync(
    join(LIBHEIF_DIR, "fuzzing", "data", "corpus", "avif32-mini.heif"),
    join(miniDir, "avif32-mini.heif"),
  );
  console.log("deps/testimages: installing HEVC sequence fixtures…");
  for (const name of HEVC_SEQUENCE_SAMPLES) {
    const dest = join(hevcSequenceDir, name);
    if (!existsSync(dest) || opts.force)
      await download(`${HEVC_SEQUENCE_BASE}/${name}`, dest);
  }
  console.log("deps/testimages: generating dependent-slice HEIC fixture…");
  generateDependentSliceHeif(
    join(hevcSequenceDir, "DSLICE_A_HHI_5.bit"),
    join(miniDir, "dependent-slices.heic"),
  );
  generateDependentSliceHeif(
    join(hevcSequenceDir, "DSLICE_B_HHI_5.bit"),
    join(miniDir, "dependent-slices-wpp.heic"),
  );
  writeFileSync(stampPath, STAMP_WANT);
  console.log(`deps/testimages: ready (${STAMP_WANT})`);
}

export async function getDeps(opts: { forceTestImages?: boolean } = {}): Promise<void> {
  mkdirSync(DEPS_DIR, { recursive: true });
  for (const { url, dir } of REPOS) {
    const path = join(DEPS_DIR, dir);
    if (existsSync(path)) continue;
    console.log(`deps: cloning ${url}`);
    await $`git clone --depth 1 ${url} ${path}`;
  }
  await ensureTestImages({ force: !!opts.forceTestImages });
}

if (import.meta.main) {
  const force = process.argv.includes("-force");
  await getDeps({ forceTestImages: force });
  console.log(
    "deps: ready (heic, dav1d, libheif, libde265, zlib, brotli, heif_conformance, testimages)",
  );
}
