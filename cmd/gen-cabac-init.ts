// Extract INIT_VALUES* from deps/heic cabac.rs → src/hevc_cabac_init.inc
import { readFileSync, writeFileSync } from "fs";
import { join } from "path";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");
const t = readFileSync(join(ROOT, "deps/heic/src/hevc/cabac.rs"), "utf8");

function grabArray(marker: string): number[] {
  const a = t.indexOf(marker);
  if (a < 0) throw new Error("missing " + marker);
  const openEq = t.indexOf("= [", a);
  if (openEq < 0) throw new Error("no = [ after " + marker);
  const open = openEq + 2; // points at '['
  let depth = 0;
  let i = open;
  for (; i < t.length; i++) {
    if (t[i] === "[") depth++;
    else if (t[i] === "]") {
      depth--;
      if (depth === 0) break;
    }
  }
  let body = t.slice(open + 1, i);
  /* Drop // comments so numbers in "(3)" annotations aren't counted. */
  body = body.replace(/\/\/[^\n]*/g, "");
  const nums = [...body.matchAll(/\b\d+\b/g)].map((m) => Number(m[0]));
  return nums;
}

const I = grabArray("pub static INIT_VALUES:");
const P = grabArray("pub static INIT_VALUES_P:");
const B = grabArray("pub static INIT_VALUES_B:");
console.log("counts", I.length, P.length, B.length);
if (I.length !== 170 || P.length !== 170 || B.length !== 170) {
  throw new Error(`expected 170 contexts each, got ${I.length}/${P.length}/${B.length}`);
}

function emit(name: string, arr: number[]): string {
  let s = `static const uint8_t ${name}[HEIC_NUM_CONTEXTS] = {\n`;
  for (let i = 0; i < arr.length; i++) {
    if (i % 16 === 0) s += "    ";
    s += String(arr[i]).padStart(3) + (i + 1 < arr.length ? ", " : "");
    if (i % 16 === 15 || i + 1 === arr.length) s += "\n";
  }
  s += "};\n\n";
  return s;
}

const out =
  "/* auto-generated from deps/heic/src/hevc/cabac.rs — do not edit */\n" +
  emit("HEIC_CABAC_INIT_I", I) +
  emit("HEIC_CABAC_INIT_P", P) +
  emit("HEIC_CABAC_INIT_B", B);
writeFileSync(join(ROOT, "src/hevc_cabac_init.inc"), out);
console.log("wrote src/hevc_cabac_init.inc");
