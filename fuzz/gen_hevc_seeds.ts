// gen_hevc_seeds.ts -- small HEVC streams for the "hevc" fuzz target.
//
// `bun cmd/fuzz.ts -target hevc -coverage` showed that the seeds cut from
// the conformance streams (64 KB each) never reach several coding tools:
// the streams that use them spend more than 64 KB on their first picture.
// This script encodes 64x64, three-picture streams with the HM reference
// encoder, one per group of tools, into fuzz/seeds-hevc/ (tracked; a few KB
// each). cmd/fuzz.ts copies them into the corpus when it seeds it.
//
//   bun fuzz/gen_hevc_seeds.ts <HM dir> [heic_test exe]
//
// <HM dir> is an HM checkout built with -DHIGH_BITDEPTH=ON (see
// test/streams/README.md). With a heic_test executable, every stream is also
// decoded by it and compared with HM's decoder output: a seed that we decode
// differently is a decoder bug, not a seed.
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync } from "node:fs";
import { join, resolve } from "node:path";
import { tmpdir } from "node:os";

const [hmArg, heicTest] = process.argv.slice(2);
if (!hmArg) {
  console.error("usage: bun fuzz/gen_hevc_seeds.ts <HM dir> [heic_test exe]");
  process.exit(2);
}
const HM = resolve(hmArg);
const ROOT = resolve(import.meta.dir, "..");
const OUT = join(ROOT, "fuzz", "seeds-hevc");

function findExe(name: string): string {
  const walk = (dir: string): string | null => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name);
      if (e.isDirectory()) {
        const r = walk(p);
        if (r) return r;
      } else if (e.name === name || e.name === `${name}.exe`) return p;
    }
    return null;
  };
  const found = walk(join(HM, "bin"));
  if (!found) throw new Error(`${name} not found under ${HM}/bin`);
  return found;
}
const ENC = findExe("TAppEncoder");
const DEC = findExe("TAppDecoder");

const W = 64, H = 64, FRAMES = 3;
const SMALL_CTU = [
  "--MaxCUWidth=16", "--MaxCUHeight=16", "--MaxPartitionDepth=2",
  "--QuadtreeTULog2MaxSize=4",
];
const C420 = "--ChromaFormatIDC=420";

/** name -> encoder options on top of the low-delay B configuration. */
const SEEDS: Record<string, string[]> = {
  /* range extensions, 4:4:4 */
  ccp_444_8: ["--InternalBitDepth=8", "--CrossComponentPrediction=1", "-q", "22"],
  ccp_scaling_444_10: [
    "--InternalBitDepth=10", "--CrossComponentPrediction=1", "--ScalingList=1",
    "-q", "27",
  ],
  tskip_rot_rdpcm_444_8: [
    "--InternalBitDepth=8", "--TransformSkip=1", "--TransformSkipLog2MaxSize=5",
    "--ImplicitResidualDPCM=1", "--ExplicitResidualDPCM=1",
    "--ResidualRotation=1", "--SingleSignificanceMapContext=1",
    "--GolombRiceParameterAdaptation=1", "-q", "12",
  ],
  lossless_444_8: [
    "--InternalBitDepth=8", "--TransquantBypassEnable=1",
    "--CUTransquantBypassFlagForce=1", "--ImplicitResidualDPCM=1",
    "--ExplicitResidualDPCM=1", "--ResidualRotation=1", "-q", "22",
  ],
  extprec_wp_444_16: [
    "--InternalBitDepth=16", "--ExtendedPrecision=1", "--WeightedPredP=1",
    "--WeightedPredB=1", "-q", "12",
  ],
  mono_400_12: ["--ChromaFormatIDC=400", "--InternalBitDepth=12", "-q", "27"],
  c422_10: ["--ChromaFormatIDC=422", "--InternalBitDepth=10", "-q", "27"],
  /* version 1 tools, 4:2:0 8-bit */
  scaling_420_8: [C420, "--InternalBitDepth=8", "--ScalingList=1", "-q", "30"],
  pcm_420_8: [
    C420, "--InternalBitDepth=8", "--PCMEnabledFlag=1", "--PCMLog2MaxSize=5",
    "--PCMLog2MinSize=3", "--PCMInputBitDepthFlag=0", "-q", "2",
  ],
  tiles_420_8: [
    C420, "--InternalBitDepth=8", ...SMALL_CTU, "--TileUniformSpacing=1",
    "--NumTileColumnsMinus1=1", "--NumTileRowsMinus1=1", "-q", "30",
  ],
  wpp_420_8: [
    C420, "--InternalBitDepth=8", ...SMALL_CTU, "--WaveFrontSynchro=1",
    "-q", "30",
  ],
  slices_420_8: [
    C420, "--InternalBitDepth=8", ...SMALL_CTU, "--SliceMode=1",
    "--SliceArgument=6", "--SliceSegmentMode=1", "--SliceSegmentArgument=3",
    "--LFCrossSliceBoundaryFlag=1", "-q", "30",
  ],
  cip_amp_tskip_420_8: [
    C420, "--InternalBitDepth=8", "--ConstrainedIntraPred=1", "--AMP=1",
    "--TransformSkip=1", "--SignHideFlag=1", "-q", "30",
  ],
  wp_420_10: [
    C420, "--InternalBitDepth=10", "--WeightedPredP=1", "--WeightedPredB=1",
    "-q", "30",
  ],
};

async function run(argv: string[]): Promise<{ rc: number; out: string }> {
  const proc = Bun.spawn(argv, { stdout: "pipe", stderr: "pipe" });
  const [out, err] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
  ]);
  return { rc: await proc.exited, out: out + err };
}

const work = join(tmpdir(), `heic-seeds-${process.pid}`);
mkdirSync(work, { recursive: true });
mkdirSync(OUT, { recursive: true });

const yuv = join(work, "in.yuv");
{
  const r = await run([
    "bun", join(ROOT, "test", "streams", "gen_yuv.ts"), "hbd16", yuv,
    String(W), String(H), String(FRAMES),
  ]);
  if (r.rc !== 0) throw new Error(`gen_yuv failed:\n${r.out}`);
}

let failed = 0;
for (const [name, opts] of Object.entries(SEEDS)) {
  const bit = join(OUT, `${name}.bit`);
  const enc = await run([
    ENC, "-c", join(HM, "cfg", "encoder_lowdelay_main.cfg"), "-i", yuv,
    "-wdt", String(W), "-hgt", String(H), "-fr", "30", "-f", String(FRAMES),
    "--InputBitDepth=16", "--InputChromaFormat=444", "--Profile=main-RExt",
    "--SEIDecodedPictureHash=0", ...opts, "-b", bit,
    "-o", join(work, "rec.yuv"), // HM otherwise drops rec.yuv in the cwd
  ]);
  if (enc.rc !== 0 || !existsSync(bit)) {
    failed++;
    rmSync(bit, { force: true });
    const why = enc.out.split("\n").filter((l) => /error/i.test(l)).slice(0, 2).join(" | ");
    console.log(`[fail] ${name}: encoder: ${why || `exit ${enc.rc}`}`);
    continue;
  }
  let verdict = "";
  if (heicTest) {
    const ref = join(work, `${name}-hm.yuv`);
    const ours = join(work, `${name}-ours.yuv`);
    const dec = await run([DEC, "-b", bit, "-o", ref, "-d", "0"]);
    const our = await run([resolve(heicTest), "-hevc-sequence", "-out", ours, bit]);
    if (dec.rc !== 0 || !existsSync(ref)) verdict = "  HM DECODER FAILED";
    else if (our.rc !== 0 || !existsSync(ours)) verdict = "  WE FAIL TO DECODE";
    else if (!readFileSync(ref).equals(readFileSync(ours))) verdict = "  DIFFERS FROM HM";
    else verdict = "  = HM";
    if (verdict !== "  = HM") failed++;
  }
  console.log(`${name}.bit ${statSync(bit).size} bytes${verdict}`);
}
rmSync(work, { recursive: true, force: true });
process.exit(failed ? 1 : 0);
