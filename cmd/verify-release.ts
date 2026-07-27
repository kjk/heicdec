// verify-release.ts — release-quality integration smoke (no oracle required).
//
//   bun cmd/verify-release.ts
//
// Exercises:
//   1. SumatraPDF surface (size / BGRA / EXIF) via heic_test -sumatra
//   2. Custom allocator + abort via heic_test -api
//   3. max_memory_bytes via heic_test -memory-limit
//   4. Amalgamation rebuild + compile check (build-dist)
//   5. WASM drop rebuild + sample decode (verify-wasm)
//
// Does not run the full libheif corpus (use tests.ts -all for that).
import { $ } from "bun";
import { existsSync } from "fs";
import { join } from "path";
import { build, defaultUseClang } from "./build";
import { getDeps } from "./get-deps";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");

async function run(label: string, cmd: string): Promise<void> {
  console.log(`\n=== ${label} ===`);
  console.log(`+ ${cmd}`);
  const r = await $`${{ raw: cmd }}`.nothrow().cwd(ROOT);
  if ((r.exitCode ?? 1) !== 0) {
    console.error(`FAILED: ${label}`);
    process.exit(1);
  }
}

await getDeps();
/* Full library surface for Sumatra/API (HEVC + AV1); oracle not required. */
const harness = await build(defaultUseClang, { withDav1d: true });
const h = harness.replaceAll("\\", "/");

const sample = "deps/heic/testdata/libheif-examples/example.heic";
const single = existsSync(join(ROOT, "deps/heic/testdata/features/single.heic"))
  ? "deps/heic/testdata/features/single.heic"
  : sample;
const hdr = existsSync(join(ROOT, "deps/heic/testdata/apple-hdr/hdr-sample.heic"))
  ? "deps/heic/testdata/apple-hdr/hdr-sample.heic"
  : sample;

await run("memory-limit", `"${h}" -memory-limit`);
await run("sumatra surface (example)", `"${h}" -sumatra ${sample}`);
await run("sumatra surface (hdr)", `"${h}" -sumatra ${hdr}`);
await run("api allocator+abort", `"${h}" -api ${sample}`);
await run("amalgamation + wasm build", `bun cmd/build-dist.ts`);
await run("wasm decode smoke", `bun cmd/verify-wasm.ts ${single}`);

console.log("\nverify-release: all checks passed");
console.log(`  harness: ${h}`);
console.log("  next: bun cmd/tests.ts -all   # full pixel oracle (optional)");
