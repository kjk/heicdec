// run-wasm-demo.ts -- serve the generated WebAssembly demo over HTTP.
//
//   bun cmd/run-wasm-demo.ts
//   bun cmd/run-wasm-demo.ts -port 8080
import path from "node:path";

const ROOT = path.resolve(import.meta.dir, "..");
const WEB_ROOT = path.join(ROOT, "dist", "wasm");
const args = process.argv.slice(2);
const portArg = args.indexOf("-port");
const port = portArg >= 0 ? Number(args[portArg + 1]) : 8000;

if (!Number.isInteger(port) || port < 1 || port > 65535)
  throw new Error("usage: bun cmd/run-wasm-demo.ts [-port 1..65535]");

const contentTypes: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".wasm": "application/wasm",
};

const server = Bun.serve({
  port,
  async fetch(req) {
    const url = new URL(req.url);
    let name: string;
    try {
      name = decodeURIComponent(url.pathname === "/" ? "demo.html" : url.pathname.slice(1));
    } catch {
      return new Response("Bad request\n", { status: 400 });
    }
    const filePath = path.resolve(WEB_ROOT, name);
    if (filePath !== WEB_ROOT && !filePath.startsWith(WEB_ROOT + path.sep))
      return new Response("Forbidden\n", { status: 403 });

    const file = Bun.file(filePath);
    if (!(await file.exists()))
      return new Response("Not found\n", { status: 404 });
    return new Response(file, {
      headers: {
        "Content-Type":
          contentTypes[path.extname(filePath).toLowerCase()] ??
          "application/octet-stream",
      },
    });
  },
});

console.log(`HEIC WebAssembly demo: http://localhost:${server.port}/`);
