// Minimal ISOBMFF box dump for fixture debugging.
//
//   bun cmd/parse_boxes.ts <file.heic|avif>
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

function boxes(
  data: Uint8Array,
  start = 0,
  end = data.length,
  indent = 0,
): void {
  let pos = start;
  const pad = " ".repeat(indent);
  while (pos + 8 <= end) {
    let size = rb32(data, pos);
    const typ = fourcc(data, pos + 4);
    let hdr = 8;
    if (size === 1) {
      // 64-bit size — rare; skip deep parse
      size = Number(
        (BigInt(rb32(data, pos + 8)) << 32n) | BigInt(rb32(data, pos + 12)),
      );
      hdr = 16;
    } else if (size === 0) size = end - pos;
    if (size < hdr || pos + size > end) {
      console.log(`${pad}BAD size=${size} at ${pos}`);
      break;
    }
    console.log(`${pad}${typ} @ ${pos} size=${size}`);
    const containers = new Set([
      "moov",
      "trak",
      "mdia",
      "minf",
      "stbl",
      "meta",
      "iprp",
      "ipco",
      "dinf",
      "dref",
    ]);
    if (containers.has(typ)) {
      const off = typ === "meta" ? 12 : 8;
      boxes(data, pos + off, pos + size, indent + 2);
    } else if (typ === "iinf") {
      const ver = data[pos + 8]!;
      let cpos = pos + 12;
      let n: number;
      if (ver === 0) {
        n = rb16(data, cpos);
        cpos += 2;
      } else {
        n = rb32(data, cpos);
        cpos += 4;
      }
      console.log(`${pad}  entries=${n}`);
      boxes(data, cpos, pos + size, indent + 2);
    } else if (
      [
        "iloc",
        "pitm",
        "iref",
        "ipma",
        "infe",
        "ispe",
        "av1C",
        "pixi",
        "colr",
        "auxC",
        "hdlr",
        "idim",
        "grid",
      ].includes(typ)
    ) {
      const payload = data.subarray(pos + 8, pos + size);
      if (typ === "pitm" && payload.length >= 6)
        console.log(`${pad}  id=${rb16(payload, 4)}`);
      if (typ === "infe" && payload.length >= 12) {
        const ver = payload[0]!;
        if (ver >= 2) {
          const iid = rb16(payload, 4);
          const itype = fourcc(payload, 8);
          console.log(`${pad}  item_id=${iid} type=${itype}`);
        }
      }
      if (typ === "ispe" && payload.length >= 12)
        console.log(`${pad}  ${rb32(payload, 4)}x${rb32(payload, 8)}`);
      if (typ === "av1C")
        console.log(
          `${pad}  av1C len=${payload.length} head=${hex(payload, 4)}`,
        );
    }
    pos += size;
  }
}

const path =
  process.argv[2] ??
  join(ROOT, "deps/testimages/avif/fox.profile0.8bpc.yuv420.avif");
boxes(new Uint8Array(readFileSync(path)));
