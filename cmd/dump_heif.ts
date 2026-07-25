// Dump top-level HEIF box tree (and a few property details).
//
//   bun cmd/dump_heif.ts <file.heic>
import { readFileSync } from "fs";
import { join } from "path";

const ROOT = `${import.meta.dir}/..`.replaceAll("\\", "/");

function rb32(d: Uint8Array, o: number): number {
  return ((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0;
}
function rb16(d: Uint8Array, o: number): number {
  return (d[o] << 8) | d[o + 1];
}
function fourcc(d: Uint8Array, o: number): string {
  return String.fromCharCode(d[o], d[o + 1], d[o + 2], d[o + 3]);
}
function hex(d: Uint8Array, n: number): string {
  return [...d.subarray(0, n)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function walk(
  data: Uint8Array,
  start: number,
  end: number,
  indent = 0,
): void {
  let off = start;
  const pad = " ".repeat(indent);
  while (off + 8 <= end) {
    let size = rb32(data, off);
    const typ = fourcc(data, off + 4);
    let hdr = 8;
    if (size === 1) {
      size = Number(
        (BigInt(rb32(data, off + 8)) << 32n) | BigInt(rb32(data, off + 12)),
      );
      hdr = 16;
    } else if (size === 0) size = end - off;
    if (size < hdr || off + size > end) break;
    console.log(`${pad}${typ} size=${size}`);
    const body = off + hdr;
    if (typ === "meta") walk(data, body + 4, off + size, indent + 2);
    else if (
      ["iprp", "ipco", "moov", "trak", "mdia", "minf", "stbl"].includes(typ)
    )
      walk(data, body, off + size, indent + 2);
    else if (typ === "ispe" && size >= 20) {
      const w = rb32(data, body + 4),
        h = rb32(data, body + 8);
      console.log(`${pad}  -> ${w}x${h}`);
    } else if (typ === "irot" && size >= 9)
      console.log(
        `${pad}  -> raw=${data[body]} bits=${data[body]! & 3}`,
      );
    else if (typ === "imir" && size >= 9)
      console.log(`${pad}  -> axis=${data[body]! & 1}`);
    else if (typ === "clap" && size >= 40) {
      const vals: number[] = [];
      for (let i = 0; i < 8; i++) vals.push(rb32(data, body + i * 4));
      console.log(`${pad}  -> ${vals.join(",")}`);
    } else if (typ === "ipma")
      console.log(
        `${pad}  -> ${hex(data.subarray(body), Math.min(48, size - hdr))}`,
      );
    else if (typ === "infe" && size >= 16) {
      const ver = data[body]!;
      if (ver >= 2) {
        const itemId =
          ver === 2 ? rb16(data, body + 4) : rb32(data, body + 4);
        const base = body + (ver === 2 ? 6 : 8);
        const itemType = fourcc(data, base + 2);
        console.log(`${pad}  -> id=${itemId} type=${itemType}`);
      }
    }
    off += size;
  }
}

const path =
  process.argv[2] ??
  join(ROOT, "deps/heic/testdata/features/irot90.heic");
const data = new Uint8Array(readFileSync(path));
console.log(path);
walk(data, 0, data.length);
