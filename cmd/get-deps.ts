// get-deps.ts -- fetch reference checkouts (corpus + oracles) and local fixtures.
//
//   bun cmd/get-deps.ts
//   bun cmd/get-deps.ts -force              # re-download testimages
//   bun cmd/get-deps.ts -skip-testimages    # repos only (CI smoke path)
//   HEIC_SKIP_TESTIMAGES=1 bun cmd/get-deps.ts
//
// Checks out each repo in deps/ at the commit pinned in REPOS (an existing
// checkout at another commit is moved to the pin, and the oracle builds made
// from it under out/ are dropped). deps/ is gitignored.
// Also fills deps/testimages/ with AVIF samples, official HEIF Mini samples,
// HEVC FATE/ITU sequences, and regenerated grid/alpha + unci block fixtures
// (no checked-in binaries). Downloads retry with backoff (FATE rate-limits).
import { $ } from "bun";
import {
  copyFileSync,
  existsSync,
  mkdirSync,
  readFileSync,
  rmSync,
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
import { generateRegressFixtures } from "./gen_regress_fixtures";

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

/**
 * Every repo is pinned: the corpus, the oracles and the md5/mse pins in
 * tests.ts all depend on the exact commit, so "whatever HEAD was at clone
 * time" made results differ between machines and CI caches.
 * To move a pin: put the new commit here, run `bun cmd/tests.ts -all`.
 *   builds  = out/ directories built from this repo (dropped when it moves)
 *   patched = files build.ts edits in place (restored before moving)
 */
type Repo = {
  url: string;
  dir: string;
  rev: string;
  builds?: string[];
  patched?: string[];
};
const REPOS: Repo[] = [
  // Rust source being ported + primary test corpus (testdata/)
  {
    url: "https://github.com/imazen/heic",
    dir: "heic",
    rev: "ad3c10c585156c11755246098b2c3144e570c795",
  },
  // AV1 decoder we link (C). Not re-ported.
  {
    url: "https://code.videolan.org/videolan/dav1d.git",
    dir: "dav1d",
    rev: "2a0775ca5ea055450bdfbfe81758a3c9e001f58a",
    builds: ["dav1d_build", "libheif_build"],
  },
  // Pixel/oracle + API inspiration (needs libde265 + dav1d)
  {
    url: "https://github.com/strukturag/libheif",
    dir: "libheif",
    rev: "12a259a5882dd99d45ec5fc77e296a3b620d1e2e",
    builds: ["libheif_build"],
    patched: ["cmake/modules/FindBrotli.cmake"],
  },
  // HEVC decoder plugin for libheif oracle
  {
    url: "https://github.com/strukturag/libde265",
    dir: "libde265",
    rev: "4fb298ec1af0686211dc2def73f4f41c2ce19ce5",
    builds: ["libde265_build", "libheif_build"],
  },
  // unci zlib/deflate (ISO 23001-17 generic compression)
  {
    url: "https://github.com/madler/zlib.git",
    dir: "zlib",
    rev: "617e19a81f38c15ff04de9f86882f42ad8a8ffd1",
    builds: ["zlib_build", "libheif_build"],
  },
  // unci brotli
  {
    url: "https://github.com/google/brotli.git",
    dir: "brotli",
    rev: "fed075f48eaffa447243a8e0452a88b103506118",
    builds: ["brotli_build", "libheif_build"],
  },
  // Nokia HEIF conformance candidates (C001.heic …) — HEVC-era HEIF
  {
    url: "https://github.com/nokiatech/heif_conformance.git",
    dir: "heif_conformance",
    rev: "6ff1e33481d325296d8a787432453ef3fa91f591",
  },
];

/** Check out `repo` at its pinned commit; returns true if the tree moved. */
async function ensureRepo(repo: Repo): Promise<boolean> {
  const path = join(DEPS_DIR, repo.dir);
  if (!existsSync(join(path, ".git"))) {
    console.log(`deps: fetching ${repo.url} @ ${repo.rev.slice(0, 12)}`);
    mkdirSync(path, { recursive: true });
    await $`git -C ${path} init -q`;
    await $`git -C ${path} remote add origin ${repo.url}`;
  } else {
    const head = (await $`git -C ${path} rev-parse HEAD`.nothrow().quiet())
      .stdout.toString().trim();
    if (head === repo.rev) return false;
    console.log(
      `deps: ${repo.dir} ${head.slice(0, 12)} -> pinned ${repo.rev.slice(0, 12)}`,
    );
    for (const f of repo.patched ?? [])
      await $`git -C ${path} checkout -q -- ${f}`.nothrow().quiet();
  }
  await $`git -C ${path} fetch -q --depth 1 origin ${repo.rev}`;
  /* No -f: local edits that conflict with the pinned tree must stop here
     with git's own message instead of being thrown away. */
  await $`git -C ${path} checkout -q --detach FETCH_HEAD`;
  for (const b of repo.builds ?? [])
    rmSync(join(ROOT, "out", b), { recursive: true, force: true });
  return true;
}

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
  "v44-avif-fox+grid+alpha+meta-moov-sequence+sequence-alpha;unci-block;mini-hevc+av1;hevc-sequences+pcm+dependent-slices+wpp+wpp-main10+deltaqp-ab+slist+persist-rparam+transquant-bypass+transform-skip+rext-tools+ccp+chroma-qp-heif+cabac-align+extprec+highprec-wp+sao-scale+constrained-intra+loop-filter-boundaries+slice-deblock+sao-diagonal-boundaries+vixs-deblock+parallel-merge+cra-poc+rext-rdpcm+amvp+mvclip+mvedge+main422+main422b+family-coverage+exact-pins+rpsd-rap-nooutprior+rext-conformance+regress";
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
  "CIP_A_Panasonic_3.bit",
  "cip_B_NEC_3.bit",
  "CIP_C_Panasonic_2.bit",
  "DELTAQP_A_BRCM_4.bit",
  "DELTAQP_B_SONY_3.bit",
  "DELTAQP_C_SONY_3.bit",
  "SLIST_A_Sony_4.bit",
  "SLIST_B_Sony_8.bit",
  "SLIST_C_Sony_3.bit",
  "SLIST_D_Sony_9.bit",
  "PERSIST_RPARAM_A_RExt_Sony_3.bit",
  "IPRED_A_docomo_2.bit",
  "IPRED_B_Nokia_3.bit",
  "IPRED_C_Mitsubishi_3.bit",
  "LTRPSPS_A_Qualcomm_1.bit",
  "MAXBINS_A_TI_4.bit",
  "MAXBINS_B_TI_4.bit",
  "MAXBINS_C_TI_4.bit",
  "MVCLIP_A_qualcomm_3.bit",
  "MVDL1ZERO_A_docomo_3.bit",
  "MVEDGE_A_qualcomm_3.bit",
  "POC_A_Bossen_3.bit",
  "RPLM_A_qualcomm_4.bit",
  "RPS_A_docomo_4.bit",
  "RPS_B_qualcomm_5.bit",
  "RPS_C_ericsson_5.bit",
  "RPS_D_ericsson_6.bit",
  "RPS_E_qualcomm_5.bit",
  "RPS_F_docomo_1.bit",
  "RAP_A_docomo_4.bit",
  "RAP_B_Bossen_1.bit",
  "NoOutPrior_A_Qualcomm_1.bit",
  "NoOutPrior_B_Qualcomm_1.bit",
  "RQT_A_HHI_4.bit",
  "RQT_B_HHI_4.bit",
  "RQT_C_HHI_4.bit",
  "SAO_A_MediaTek_4.bit",
  "SAO_B_MediaTek_5.bit",
  "SAO_C_Samsung_5.bit",
  "SAO_D_Samsung_5.bit",
  "SAO_E_Canon_4.bit",
  "SAO_F_Canon_3.bit",
  "SAO_G_Canon_3.bit",
  "TILES_A_Cisco_2.bit",
  "TILES_B_Cisco_1.bit",
  "TMVP_A_MS_3.bit",
  "TUSIZE_A_Samsung_1.bit",
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
  "WPP_A_ericsson_MAIN_2.bit",
  "WPP_B_ericsson_MAIN_2.bit",
  "WPP_C_ericsson_MAIN_2.bit",
  "WPP_D_ericsson_MAIN_2.bit",
  "WPP_E_ericsson_MAIN_2.bit",
  "WPP_F_ericsson_MAIN_2.bit",
  "WPP_A_ericsson_MAIN10_2.bit",
  "WPP_B_ericsson_MAIN10_2.bit",
  "WPP_C_ericsson_MAIN10_2.bit",
  "WPP_D_ericsson_MAIN10_2.bit",
  "WPP_E_ericsson_MAIN10_2.bit",
  "WPP_F_ericsson_MAIN10_2.bit",
  "WPP_HIGH_TP_444_8BIT_RExt_Apple_2.bit",
  "Main_422_10_B_RExt_Sony_1.bin",
];

function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Transient network failures (rate limits, ConnectionRefused, …) are retried. */
async function download(
  url: string,
  dest: string,
  opts: { attempts?: number } = {},
): Promise<void> {
  const attempts = opts.attempts ?? 6;
  let lastErr: unknown;
  for (let i = 0; i < attempts; i++) {
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`GET ${url} → ${res.status}`);
      const buf = new Uint8Array(await res.arrayBuffer());
      writeFileSync(dest, buf);
      console.log(`deps/testimages: downloaded ${dest} (${buf.length} bytes)`);
      return;
    } catch (err) {
      lastErr = err;
      if (i + 1 >= attempts) break;
      const delayMs = Math.min(30_000, 1000 * 2 ** i);
      console.log(
        `deps/testimages: download retry ${i + 1}/${attempts - 1} in ${delayMs}ms (${url})`,
      );
      await sleep(delayMs);
    }
  }
  throw lastErr instanceof Error
    ? lastErr
    : new Error(`GET ${url} failed: ${String(lastErr)}`);
}

/**
 * The JCT-VC range-extensions conformance streams (HEVC_REXT_BASE). Every zip
 * carries the MD5 of the reference decoder's output next to the bitstream;
 * tests.ts decodes each stream and compares against that MD5, so nothing is
 * pinned by hand.
 */
/** HEIC_SKIP_REXT=1: neither download nor run the RExt conformance set (CI
 * runs it on one platform; the decoder is the same C code on all of them). */
export const SKIP_REXT = process.env.HEIC_SKIP_REXT === "1";

export const HEVC_REXT_CONFORMANCE = [
  "ADJUST_IPRED_ANGLE_A_RExt_Mitsubishi_2",
  "Bitdepth_A_RExt_Sony_1",
  "Bitdepth_B_RExt_Sony_1",
  "CCP_10bit_RExt_QCOM_1",
  "CCP_12bit_RExt_QCOM_1",
  "CCP_8bit_RExt_QCOM_1",
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_10BIT_RExt_Sony_1",
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_12BIT_RExt_Sony_1",
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_16BIT_RExt_Sony_1",
  "EXTPREC_HIGHTHROUGHPUT_444_16_INTRA_8BIT_RExt_Sony_1",
  "EXTPREC_MAIN_444_16_INTRA_10BIT_RExt_Sony_1",
  "EXTPREC_MAIN_444_16_INTRA_12BIT_RExt_Sony_1",
  "EXTPREC_MAIN_444_16_INTRA_16BIT_RExt_Sony_1",
  "EXTPREC_MAIN_444_16_INTRA_8BIT_RExt_Sony_1",
  "ExplicitRdpcm_A_BBC_1",
  "ExplicitRdpcm_B_BBC_2",
  "GENERAL_10b_420_RExt_Sony_1",
  "GENERAL_10b_422_RExt_Sony_1",
  "GENERAL_10b_444_RExt_Sony_2",
  "GENERAL_12b_400_RExt_Sony_1",
  "GENERAL_12b_420_RExt_Sony_1",
  "GENERAL_12b_422_RExt_Sony_1",
  "GENERAL_12b_444_RExt_Sony_2",
  "GENERAL_16b_400_RExt_Sony_1",
  "GENERAL_16b_444_RExt_Sony_2",
  "GENERAL_16b_444_highThroughput_RExt_Sony_2",
  "GENERAL_8b_400_RExt_Sony_1",
  "GENERAL_8b_420_RExt_Sony_1",
  "GENERAL_8b_444_RExt_Sony_2",
  "IPCM_A_RExt_NEC_2",
  "IPCM_B_RExt_NEC",
  "Main_422_10_A_RExt_Sony_2",
  "Main_422_10_B_RExt_Sony_2",
  "PERSIST_RPARAM_A_RExt_Sony_3",
  "QMATRIX_A_RExt_Sony_1",
  "SAO_A_RExt_MediaTek_1",
  "TSCTX_10bit_I_RExt_SHARP_1",
  "TSCTX_10bit_RExt_SHARP_1",
  "TSCTX_12bit_I_RExt_SHARP_1",
  "TSCTX_12bit_RExt_SHARP_1",
  "TSCTX_8bit_I_RExt_SHARP_1",
  "TSCTX_8bit_RExt_SHARP_1",
  "WAVETILES_RExt_Sony_2",
  "WPP_AND_TILE_10Bit422Test_HIGH_TP_444_10BIT_RExt_Apple_2",
  "WPP_AND_TILE_AND_CABAC_BYPASS_ALIGN_0_HIGH_TP_444_14BIT_RExt_Apple_2",
  "WPP_AND_TILE_AND_CABAC_BYPASS_ALIGN_1_HIGH_TP_444_14BIT_RExt_Apple_2",
  "WPP_AND_TILE_AND_CABAC_EXT_PREC_1_HIGH_TP_444_14BIT_RExt_Apple_2",
  "WPP_AND_TILE_HIGH_TP_444_8BIT_RExt_Apple_2",
  "WPP_HIGH_TP_444_8BIT_RExt_Apple_2",
] as const;

/** deps/testimages/hevc_rext/<name>.bit + <name>.md5 (the zip's MD5 text). */
export const HEVC_REXT_DIR = join(TESTIMAGES_DIR, "hevc_rext");

/** Extract the first entry whose name ends with `entry` (string) or matches
 *  it (RegExp); directory entries never match. With `all`, every matching
 *  entry is written, one after the other. */
function extractZipEntry(
  zipPath: string,
  entry: string | RegExp,
  dest: string,
  all = false,
): void {
  const found: Uint8Array[] = [];
  const wanted = (name: string) =>
    !name.endsWith("/")
    && (typeof entry === "string" ? name.endsWith(entry) : entry.test(name));
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
    if (wanted(name)) {
      const localNameLen = view.getUint16(localOffset + 26, true);
      const localExtraLen = view.getUint16(localOffset + 28, true);
      const dataOffset = localOffset + 30 + localNameLen + localExtraLen;
      const compressed = zip.subarray(dataOffset, dataOffset + compressedSize);
      if (method === 0)
        found.push(compressed);
      else if (method === 8)
        found.push(inflateRawSync(compressed));
      else
        throw new Error(`unsupported ZIP method ${method}: ${zipPath}`);
      if (!all) break;
    }
    pos += 46 + nameLen + extraLen + commentLen;
  }
  if (!found.length)
    throw new Error(`ZIP entry ${entry} not found in ${zipPath}`);
  writeFileSync(
    dest,
    all
      ? Buffer.concat(found.flatMap((f) => [f, Buffer.from("\n")]))
      : found[0]!,
  );
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
    (SKIP_REXT ? [] : HEVC_REXT_CONFORMANCE).every(
      (n) =>
        existsSync(join(HEVC_REXT_DIR, `${n}.bit`))
        && existsSync(join(HEVC_REXT_DIR, `${n}.md5`)),
    ) &&
    existsSync(join(root, "regress", "two_pps_grid.heic")) &&
    existsSync(join(root, "regress", "two_ipma_grid.heic")) &&
    existsSync(join(root, "regress", "iovl_red_fill.heic")) &&
    existsSync(join(root, "regress", "essential_unknown.heic")) &&
    existsSync(join(root, "regress", "_info", "grid_no_ispe.heic")) &&
    existsSync(join(root, "regress", "_info", "aux_no_auxc.heic")) &&
    existsSync(join(unciDir, "mono_alpha_comp.heif")) &&
    existsSync(join(unciDir, "rgb8_truncated.heif")) &&
    existsSync(join(unciDir, "rgb8_zlib_bomb.heif")) &&
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
  mkdirSync(HEVC_REXT_DIR, { recursive: true });

  for (const name of SKIP_REXT ? [] : HEVC_REXT_CONFORMANCE) {
    const bit = join(HEVC_REXT_DIR, `${name}.bit`);
    const md5 = join(HEVC_REXT_DIR, `${name}.md5`);
    if (existsSync(bit) && existsSync(md5) && !opts.force) continue;
    const zip = join(HEVC_REXT_DIR, `${name}.zip`);
    console.log(`deps/testimages: fetching ${name}.zip…`);
    await download(`${HEVC_REXT_BASE}/${name}.zip`, zip);
    extractZipEntry(zip, /\.(bit|bin)$/i, bit);
    /* Some zips keep the bitstream MD5 and the YUV MD5 in separate files. */
    extractZipEntry(zip, /md5[^/]*$/i, md5, true);
    rmSync(zip, { force: true });
  }

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
  /* Container-level regression files derived from a corpus image. */
  generateRegressFixtures(
    join(HEIC_RUST_DIR, "testdata", "features", "single.heic"),
    join(root, "regress"),
  );
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
    if (!existsSync(dest) || opts.force) {
      await download(`${HEVC_SEQUENCE_BASE}/${name}`, dest);
      // FATE suite rate-limits rapid sequential GETs (ConnectionRefused on CI).
      await sleep(150);
    }
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

/* build(), buildRef() and each oracle builder call getDeps(); one check per
   process is enough (a dozen repeats cost ~100 stat calls and a line of
   output each). */
let depsReady: Promise<void> | null = null;

export function getDeps(
  opts: { forceTestImages?: boolean; skipTestImages?: boolean } = {},
): Promise<void> {
  if (opts.forceTestImages) return getDepsOnce(opts);
  depsReady ??= getDepsOnce(opts);
  return depsReady;
}

async function getDepsOnce(
  opts: { forceTestImages?: boolean; skipTestImages?: boolean } = {},
): Promise<void> {
  mkdirSync(DEPS_DIR, { recursive: true });
  for (const repo of REPOS) await ensureRepo(repo);
  const skip =
    opts.skipTestImages
    || process.env.HEIC_SKIP_TESTIMAGES === "1"
    || process.env.HEIC_SKIP_TESTIMAGES === "true";
  if (skip) {
    console.log(
      "deps: skipping testimages (HEIC_SKIP_TESTIMAGES / -skip-testimages)",
    );
    return;
  }
  await ensureTestImages({ force: !!opts.forceTestImages });
}

if (import.meta.main) {
  const force = process.argv.includes("-force");
  const skipTestImages = process.argv.includes("-skip-testimages");
  await getDeps({ forceTestImages: force, skipTestImages });
  console.log(
    "deps: ready (heic, dav1d, libheif, libde265, zlib, brotli, heif_conformance"
      + (skipTestImages
          || process.env.HEIC_SKIP_TESTIMAGES === "1"
          || process.env.HEIC_SKIP_TESTIMAGES === "true"
        ? "; testimages skipped)"
        : ", testimages)"),
  );
}
