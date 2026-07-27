// get-deps.ts -- fetch reference checkouts (corpus + oracles) and local fixtures.
//
//   bun cmd/get-deps.ts
//
// Clones into deps/ (skipped if present). deps/ is gitignored.
// Also fills deps/testimages/ with AVIF samples, official HEIF Mini samples,
// and regenerated grid/alpha + unci block fixtures (no checked-in binaries).
import { $ } from "bun";
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  writeFileSync,
} from "fs";
import { join } from "path";
import { inflateRawSync } from "zlib";
import { generateAvifFixtures } from "./gen_avif_fixtures";
import {
  generateConstrainedIntraHevc,
  generateHevcHeif,
  generateHighPrecisionWeightingHevc,
  generateLoopFilterSlicesHevc,
  generateLoopFilterTilesHevc,
  generateSaoOffsetScaleHevc,
} from "./gen_hevc_fixtures";
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
  "v35-avif-fox+grid+alpha+meta-moov-sequence+sequence-alpha;unci-block;mini-hevc+av1;hevc-sequences+pcm+dependent-slices+wpp+transquant-bypass+transform-skip+rext-tools+ccp+chroma-qp-heif+cabac-align+extprec+highprec-wp+sao-scale+constrained-intra+loop-filter-boundaries+slice-deblock+sao-diagonal-boundaries+vixs-deblock+parallel-merge+cra-poc+rext-rdpcm+amvp+mvclip+mvedge+main422";
const HEVC_SEQUENCE_BASE = "https://fate-suite.ffmpeg.org/hevc-conformance";
const HEVC_V1_BASE =
  "https://www.itu.int/wftp3/av-arch/jctvc-site/" +
  "bitstream_exchange/draft_conformance/HEVC_v1";
const HEVC_REXT_BASE =
  "https://www.itu.int/wftp3/av-arch/jctvc-site/" +
  "bitstream_exchange/draft_conformance/RExt";
const HEVC_SAO_DIAGONAL = "SAO_H_Parabola_1.bit";
const HEVC_SAO_DIAGONAL_ZIP = "SAO_H_Parabola_1.zip";
const HEVC_REXT_TSCTX = "TSCTX_8bit_I_RExt_SHARP_1.bin";
const HEVC_REXT_TSCTX_ZIP = "TSCTX_8bit_I_RExt_SHARP_1.zip";
const HEVC_REXT_GENERAL = "GENERAL_8b_444_RExt_Sony_2.bit";
const HEVC_REXT_GENERAL_ZIP = "GENERAL_8b_444_RExt_Sony_2.zip";
const HEVC_REXT_GENERAL_SEQUENCES = [1, 2, 3, 4, 5, 6] as const;
const HEVC_REXT_CCP = "CCP_8bit_RExt_QCOM_1.bin";
const HEVC_REXT_CCP_ENTRY = "CCP_8bit_RExt_QCOM.bin";
const HEVC_REXT_CCP_ZIP = "CCP_8bit_RExt_QCOM_1.zip";
const HEVC_REXT_EXTPREC =
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_10BIT_RExt_Sony_1.bit";
const HEVC_REXT_EXTPREC_ZIP =
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_10BIT_RExt_Sony_1.zip";
const HEVC_REXT_EXTPREC_SEQUENCE0 =
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_10BIT_RExt_Sony_1-seq0.bit";
const HEVC_REXT_EXTPREC_SEQUENCE1 =
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_10BIT_RExt_Sony_1-seq1.bit";
const HEVC_REXT_MAIN422 = "Main_422_10_A_RExt_Sony_2.bin";
const HEVC_REXT_MAIN422_ZIP = "Main_422_10_A_RExt_Sony_2.zip";
const HEVC_REXT_GENERAL_422 = "GENERAL_10b_422_RExt_Sony_1.bit";
const HEVC_REXT_GENERAL_422_ZIP = "GENERAL_10b_422_RExt_Sony_1.zip";
const HEVC_REXT_GENERAL_422_SEQUENCE0 =
  "GENERAL_10b_422_RExt_Sony_1-seq0.bit";
const HEVC_REXT_GENERAL_422_SEQUENCE1 =
  "GENERAL_10b_422_RExt_Sony_1-seq1.bit";
const HEVC_HIGH_PRECISION_WEIGHTING =
  "high-precision-weighting-main10.bit";
const HEVC_SAO_OFFSET_SCALE = "sao-offset-scale-main12.bit";
const HEVC_CONSTRAINED_INTRA = "constrained-intra-main.bit";
const HEVC_LOOP_FILTER_TILES = "loop-filter-tiles-main.bit";
const HEVC_LOOP_FILTER_SLICES = "loop-filter-slices-main.bit";
const HEVC_SEQUENCE_SAMPLES = [
  "AMVP_A_MTK_4.bit",
  "AMVP_B_MTK_4.bit",
  "AMVP_C_Samsung_6.bit",
  "LTRPSPS_A_Qualcomm_1.bit",
  "MVCLIP_A_qualcomm_3.bit",
  "MVDL1ZERO_A_docomo_3.bit",
  "MVEDGE_A_qualcomm_3.bit",
  "RPLM_A_qualcomm_4.bit",
  "TMVP_A_MS_3.bit",
  "WP_A_Toshiba_3.bit",
  "WP_B_Toshiba_3.bit",
  "MERGE_A_TI_3.bit",
  "PMERGE_A_TI_3.bit",
  "PMERGE_B_TI_3.bit",
  "PMERGE_C_TI_3.bit",
  "PMERGE_D_TI_3.bit",
  "PMERGE_E_TI_3.bit",
  "DBLK_A_SONY_3.bit",
  "DBLK_A_MAIN10_VIXS_3.bit",
  "DBLK_B_SONY_3.bit",
  "DBLK_C_SONY_3.bit",
  "DBLK_D_VIXS_2.bit",
  "DBLK_E_VIXS_2.bit",
  "DBLK_F_VIXS_1.bit",
  "DBLK_F_VIXS_2.bit",
  "DBLK_G_VIXS_2.bit",
  "DSLICE_A_HHI_5.bit",
  "DSLICE_B_HHI_5.bit",
  "LS_A_Orange_2.bit",
  "ipcm_A_NEC_3.bit",
  "ipcm_B_NEC_3.bit",
  "ipcm_C_NEC_3.bit",
  "TSKIP_A_MS_3.bit",
  "TSUNEQBD_A_MAIN10_Technicolor_2.bit",
  "WPP_HIGH_TP_444_8BIT_RExt_Apple_2.bit",
];

async function download(url: string, dest: string): Promise<void> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`GET ${url} → ${res.status}`);
  const buf = new Uint8Array(await res.arrayBuffer());
  writeFileSync(dest, buf);
  console.log(`deps/testimages: downloaded ${dest} (${buf.length} bytes)`);
}

function extractZipEntry(zipPath: string, entrySuffix: string, dest: string): void {
  const zip = new Uint8Array(readFileSync(zipPath));
  const view = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  let eocd = -1;
  for (let i = zip.length - 22; i >= Math.max(0, zip.length - 65557); i--) {
    if (view.getUint32(i, true) === 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new Error(`invalid ZIP: ${zipPath}`);
  const entries = view.getUint16(eocd + 10, true);
  let pos = view.getUint32(eocd + 16, true);
  const decoder = new TextDecoder();
  for (let i = 0; i < entries; i++) {
    if (view.getUint32(pos, true) !== 0x02014b50)
      throw new Error(`invalid ZIP directory: ${zipPath}`);
    const method = view.getUint16(pos + 10, true);
    const compressedSize = view.getUint32(pos + 20, true);
    const nameLen = view.getUint16(pos + 28, true);
    const extraLen = view.getUint16(pos + 30, true);
    const commentLen = view.getUint16(pos + 32, true);
    const localOffset = view.getUint32(pos + 42, true);
    const name = decoder.decode(zip.subarray(pos + 46, pos + 46 + nameLen));
    if (name.endsWith(entrySuffix)) {
      const localNameLen = view.getUint16(localOffset + 26, true);
      const localExtraLen = view.getUint16(localOffset + 28, true);
      const dataOffset = localOffset + 30 + localNameLen + localExtraLen;
      const compressed = zip.subarray(dataOffset, dataOffset + compressedSize);
      if (method === 0)
        writeFileSync(dest, compressed);
      else if (method === 8)
        writeFileSync(dest, inflateRawSync(compressed));
      else
        throw new Error(`unsupported ZIP method ${method}: ${zipPath}`);
      return;
    }
    pos += 46 + nameLen + extraLen + commentLen;
  }
  throw new Error(`ZIP entry ${entrySuffix} not found in ${zipPath}`);
}

function findAnnexBSequenceStarts(data: Uint8Array): number[] {
  const starts: number[] = [];
  for (let i = 0; i + 5 < data.length;) {
    let prefix = 0;
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1)
      prefix = 3;
    else if (
      data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 0
      && data[i + 3] === 1
    )
      prefix = 4;
    if (prefix) {
      if (((data[i + prefix]! >>> 1) & 0x3f) === 32) starts.push(i);
      i += prefix;
    } else {
      i++;
    }
  }
  return starts;
}

function splitAnnexBSequences(
  sourcePath: string,
  outputDir: string,
  wanted: readonly number[],
): void {
  const data = new Uint8Array(readFileSync(sourcePath));
  const starts = findAnnexBSequenceStarts(data);
  if (starts.length < 7)
    throw new Error(`expected 7 RExt sequences in ${sourcePath}`);
  starts.push(data.length);
  for (const index of wanted) {
    writeFileSync(
      join(outputDir, `GENERAL_8b_444_RExt_Sony_2-seq${index}.bit`),
      data.subarray(starts[index]!, starts[index + 1]!),
    );
  }
}

function extractAnnexBSequence(
  sourcePath: string,
  destPath: string,
  index: number,
  expected: number,
): void {
  const data = new Uint8Array(readFileSync(sourcePath));
  const starts = findAnnexBSequenceStarts(data);
  if (starts.length < expected || index < 0 || index >= starts.length)
    throw new Error(`expected ${expected} HEVC sequences in ${sourcePath}`);
  starts.push(data.length);
  writeFileSync(destPath, data.subarray(starts[index]!, starts[index + 1]!));
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
    existsSync(join(miniDir, "transquant-bypass.heic")) &&
    existsSync(join(miniDir, "transform-skip.heic")) &&
    existsSync(join(miniDir, "transform-skip-main10.heic")) &&
    existsSync(join(miniDir, "transform-skip-context-rext.heic")) &&
    existsSync(join(miniDir, "chroma-qp-offset-rext.heic")) &&
    existsSync(join(hevcSequenceDir, HEVC_REXT_TSCTX)) &&
    existsSync(join(hevcSequenceDir, HEVC_REXT_CCP)) &&
    existsSync(join(hevcSequenceDir, HEVC_REXT_EXTPREC_SEQUENCE0)) &&
    existsSync(join(hevcSequenceDir, HEVC_REXT_EXTPREC_SEQUENCE1)) &&
    existsSync(join(hevcSequenceDir, HEVC_REXT_MAIN422)) &&
    existsSync(join(hevcSequenceDir, HEVC_REXT_GENERAL_422_SEQUENCE0)) &&
    existsSync(join(hevcSequenceDir, HEVC_REXT_GENERAL_422_SEQUENCE1)) &&
    existsSync(join(hevcSequenceDir, HEVC_HIGH_PRECISION_WEIGHTING)) &&
    existsSync(join(hevcSequenceDir, HEVC_SAO_OFFSET_SCALE)) &&
    existsSync(join(hevcSequenceDir, HEVC_SAO_DIAGONAL)) &&
    existsSync(join(hevcSequenceDir, HEVC_CONSTRAINED_INTRA)) &&
    HEVC_REXT_GENERAL_SEQUENCES.every((index) =>
      existsSync(join(
        hevcSequenceDir,
        `GENERAL_8b_444_RExt_Sony_2-seq${index}.bit`,
      ))
    ) &&
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
  {
    const dest = join(hevcSequenceDir, HEVC_SAO_DIAGONAL);
    const zip = join(hevcSequenceDir, HEVC_SAO_DIAGONAL_ZIP);
    if (!existsSync(dest) || opts.force) {
      await download(`${HEVC_V1_BASE}/${HEVC_SAO_DIAGONAL_ZIP}`, zip);
      extractZipEntry(zip, HEVC_SAO_DIAGONAL, dest);
      console.log(`deps/testimages: extracted ${dest}`);
    }
  }
  {
    const dest = join(hevcSequenceDir, HEVC_REXT_TSCTX);
    const zip = join(hevcSequenceDir, HEVC_REXT_TSCTX_ZIP);
    if (!existsSync(dest) || opts.force) {
      await download(`${HEVC_REXT_BASE}/${HEVC_REXT_TSCTX_ZIP}`, zip);
      extractZipEntry(zip, HEVC_REXT_TSCTX, dest);
      console.log(`deps/testimages: extracted ${dest}`);
    }
  }
  {
    const source = join(hevcSequenceDir, HEVC_REXT_GENERAL);
    const zip = join(hevcSequenceDir, HEVC_REXT_GENERAL_ZIP);
    if (
      opts.force
      || HEVC_REXT_GENERAL_SEQUENCES.some((index) =>
        !existsSync(join(
          hevcSequenceDir,
          `GENERAL_8b_444_RExt_Sony_2-seq${index}.bit`,
        ))
      )
    ) {
      await download(`${HEVC_REXT_BASE}/${HEVC_REXT_GENERAL_ZIP}`, zip);
      extractZipEntry(zip, HEVC_REXT_GENERAL, source);
      splitAnnexBSequences(
        source,
        hevcSequenceDir,
        HEVC_REXT_GENERAL_SEQUENCES,
      );
      console.log(`deps/testimages: split ${source}`);
    }
  }
  {
    const dest = join(hevcSequenceDir, HEVC_REXT_CCP);
    const zip = join(hevcSequenceDir, HEVC_REXT_CCP_ZIP);
    if (!existsSync(dest) || opts.force) {
      await download(`${HEVC_REXT_BASE}/${HEVC_REXT_CCP_ZIP}`, zip);
      extractZipEntry(zip, HEVC_REXT_CCP_ENTRY, dest);
      console.log(`deps/testimages: extracted ${dest}`);
    }
  }
  {
    const source = join(hevcSequenceDir, HEVC_REXT_EXTPREC);
    const dest0 = join(hevcSequenceDir, HEVC_REXT_EXTPREC_SEQUENCE0);
    const dest1 = join(hevcSequenceDir, HEVC_REXT_EXTPREC_SEQUENCE1);
    const zip = join(hevcSequenceDir, HEVC_REXT_EXTPREC_ZIP);
    if (!existsSync(dest0) || !existsSync(dest1) || opts.force) {
      await download(`${HEVC_REXT_BASE}/${HEVC_REXT_EXTPREC_ZIP}`, zip);
      extractZipEntry(zip, HEVC_REXT_EXTPREC, source);
      extractAnnexBSequence(source, dest0, 0, 2);
      extractAnnexBSequence(source, dest1, 1, 2);
      console.log(`deps/testimages: extracted ${dest0}, ${dest1}`);
    }
  }
  {
    const dest = join(hevcSequenceDir, HEVC_REXT_MAIN422);
    const zip = join(hevcSequenceDir, HEVC_REXT_MAIN422_ZIP);
    if (!existsSync(dest) || opts.force) {
      await download(`${HEVC_REXT_BASE}/${HEVC_REXT_MAIN422_ZIP}`, zip);
      extractZipEntry(zip, HEVC_REXT_MAIN422, dest);
      console.log(`deps/testimages: extracted ${dest}`);
    }
  }
  {
    const source = join(hevcSequenceDir, HEVC_REXT_GENERAL_422);
    const dest0 = join(hevcSequenceDir, HEVC_REXT_GENERAL_422_SEQUENCE0);
    const dest1 = join(hevcSequenceDir, HEVC_REXT_GENERAL_422_SEQUENCE1);
    const zip = join(hevcSequenceDir, HEVC_REXT_GENERAL_422_ZIP);
    if (!existsSync(dest0) || !existsSync(dest1) || opts.force) {
      await download(`${HEVC_REXT_BASE}/${HEVC_REXT_GENERAL_422_ZIP}`, zip);
      extractZipEntry(zip, HEVC_REXT_GENERAL_422, source);
      extractAnnexBSequence(source, dest0, 0, 2);
      extractAnnexBSequence(source, dest1, 1, 2);
      console.log(`deps/testimages: extracted ${dest0}, ${dest1}`);
    }
  }
  generateHighPrecisionWeightingHevc(
    join(hevcSequenceDir, HEVC_HIGH_PRECISION_WEIGHTING),
  );
  generateSaoOffsetScaleHevc(
    join(hevcSequenceDir, HEVC_SAO_OFFSET_SCALE),
  );
  generateConstrainedIntraHevc(
    join(hevcSequenceDir, HEVC_CONSTRAINED_INTRA),
  );
  generateLoopFilterTilesHevc(
    join(hevcSequenceDir, HEVC_LOOP_FILTER_TILES),
  );
  generateLoopFilterSlicesHevc(
    join(hevcSequenceDir, HEVC_LOOP_FILTER_SLICES),
  );
  console.log("deps/testimages: generating HEVC fixtures…");
  generateHevcHeif(
    join(hevcSequenceDir, "DSLICE_A_HHI_5.bit"),
    join(miniDir, "dependent-slices.heic"),
    1920,
    1080,
  );
  generateHevcHeif(
    join(hevcSequenceDir, "DSLICE_B_HHI_5.bit"),
    join(miniDir, "dependent-slices-wpp.heic"),
    1920,
    1080,
  );
  generateHevcHeif(
    join(hevcSequenceDir, "LS_A_Orange_2.bit"),
    join(miniDir, "transquant-bypass.heic"),
    416,
    240,
  );
  generateHevcHeif(
    join(hevcSequenceDir, "TSKIP_A_MS_3.bit"),
    join(miniDir, "transform-skip.heic"),
    1280,
    720,
  );
  generateHevcHeif(
    join(hevcSequenceDir, "TSUNEQBD_A_MAIN10_Technicolor_2.bit"),
    join(miniDir, "transform-skip-main10.heic"),
    1024,
    768,
    10,
    9,
  );
  generateHevcHeif(
    join(hevcSequenceDir, HEVC_REXT_TSCTX),
    join(miniDir, "transform-skip-context-rext.heic"),
    1920,
    1080,
    8,
    8,
    3,
    4,
  );
  generateHevcHeif(
    join(hevcSequenceDir, "GENERAL_8b_444_RExt_Sony_2-seq6.bit"),
    join(miniDir, "chroma-qp-offset-rext.heic"),
    400,
    384,
    8,
    8,
    3,
    4,
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
