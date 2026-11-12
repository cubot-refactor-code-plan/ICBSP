#!/usr/bin/env node
// ICBSP 接入帮助的 Web 前端：只做展示和收集选择，接入逻辑全在 tools/integrate.py（经 --json 调用）。
// 零依赖：只用 Node 自带模块，不需要 npm install。
//
//   node Middlewares/Third_Party/ICBSP/tools/web_ui/server.mjs [--host <宿主工程根目录>] [--port 8765] [--python <python 可执行文件>]
//
// 只监听 127.0.0.1。找不到 Python（>= 3.8）时页面上会给出提示。

import { createServer } from "node:http";
import { spawnSync, spawn } from "node:child_process";
import { readFile, writeFile, mkdtemp, rm } from "node:fs/promises";
import { existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname, resolve, extname } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const INTEGRATE = resolve(HERE, "..", "integrate.py");
const LIB_ROOT = resolve(HERE, "..", "..");
const MIN_PY = [3, 8];

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i > 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

// ---------- Python ----------

function findPython() {
  const tries = [];
  if (arg("--python")) tries.push([arg("--python")]);
  if (process.env.ICBSP_PYTHON) tries.push([process.env.ICBSP_PYTHON]);
  tries.push(["python3"], ["python"]);
  if (process.platform === "win32") tries.push(["py", "-3"]);
  for (const cmd of tries) {
    const r = spawnSync(cmd[0], [...cmd.slice(1), "-c", "import sys;print('%d.%d' % sys.version_info[:2])"], { encoding: "utf-8" });
    if (r.status === 0) {
      const [ma, mi] = r.stdout.trim().split(".").map(Number);
      const ok = ma > MIN_PY[0] || (ma === MIN_PY[0] && mi >= MIN_PY[1]);
      return { cmd, version: r.stdout.trim(), ok, error: ok ? "" : `Python ${r.stdout.trim()} 太旧，需要 ${MIN_PY.join(".")} 及以上` };
    }
  }
  return { cmd: null, version: null, ok: false, error: "找不到 Python（试过 --python、环境变量 ICBSP_PYTHON、python3、python）：安装 Python 3.8+，或用 --python 指定" };
}

const PY = findPython();
const HOST = resolve(arg("--host", defaultHost()));
const PORT = Number(arg("--port", "8765"));

function defaultHost() {
  // 本库作为 submodule 时，宿主是 git superproject；否则当前目录
  const r = spawnSync("git", ["-C", LIB_ROOT, "rev-parse", "--show-superproject-working-tree"], { encoding: "utf-8" });
  const out = (r.stdout || "").trim();
  return out || process.cwd();
}

// ---------- 调 integrate.py ----------

let busy = false;

async function integrate(extra, selection) {
  if (!PY.ok) return { status: 500, body: { ok: false, fatal: PY.error } };
  if (busy) return { status: 409, body: { ok: false, fatal: "上一个请求还在运行（CubeMX 重新生成可能要几十秒）" } };
  busy = true;
  let dir = null;
  try {
    const args = [...PY.cmd.slice(1), INTEGRATE, "--host", HOST, "--json", ...extra];
    if (selection) {
      dir = await mkdtemp(join(tmpdir(), "icbsp-ui-"));
      const f = join(dir, "selection.json");
      await writeFile(f, JSON.stringify(selection), "utf-8");
      args.push("--apply", f);
    }
    const { code, stdout, stderr } = await run(PY.cmd[0], args);
    try {
      const body = JSON.parse(stdout);
      return { status: 200, body };
    } catch {
      return { status: 500, body: { ok: false, fatal: `integrate.py 没有输出 JSON（退出码 ${code}）`, stderr: stderr.slice(-4000), stdout: stdout.slice(-4000) } };
    }
  } finally {
    busy = false;
    if (dir) await rm(dir, { recursive: true, force: true });
  }
}

function run(cmd, args) {
  return new Promise((res) => {
    const p = spawn(cmd, args, { cwd: HOST });
    let stdout = "", stderr = "";
    p.stdout.on("data", (d) => (stdout += d));
    p.stderr.on("data", (d) => (stderr += d));
    p.on("close", (code) => res({ code, stdout, stderr }));
    p.on("error", (e) => res({ code: -1, stdout, stderr: String(e) }));
  });
}

function commonArgs(b) {
  const a = [];
  if (b.board_dir) a.push("--board-dir", String(b.board_dir));
  if (b.app_dir) a.push("--app-dir", String(b.app_dir));
  if (b.ioc) a.push("--ioc", String(b.ioc));
  if (b.no_vscode) a.push("--no-vscode");
  if (b.no_flash) a.push("--no-flash");
  if (b.no_clang) a.push("--no-clang");
  return a;
}

// ---------- HTTP ----------

const STATIC = { "/": "index.html", "/index.html": "index.html", "/app.js": "app.js", "/style.css": "style.css" };
const TYPES = { ".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css; charset=utf-8" };

function readBody(req, limit = 1 << 20) {
  return new Promise((res, rej) => {
    let n = 0;
    const chunks = [];
    req.on("data", (c) => {
      n += c.length;
      if (n > limit) {
        rej(new Error("请求太大"));
        req.destroy();
      } else chunks.push(c);
    });
    req.on("end", () => {
      try {
        res(chunks.length ? JSON.parse(Buffer.concat(chunks).toString("utf-8")) : {});
      } catch (e) {
        rej(e);
      }
    });
  });
}

function send(res, status, body) {
  res.writeHead(status, { "Content-Type": "application/json; charset=utf-8", "Cache-Control": "no-store" });
  res.end(JSON.stringify(body));
}

const server = createServer(async (req, res) => {
  try {
    const url = new URL(req.url, "http://127.0.0.1");
    if (req.method === "GET" && STATIC[url.pathname]) {
      const f = join(HERE, STATIC[url.pathname]);
      res.writeHead(200, { "Content-Type": TYPES[extname(f)], "Cache-Control": "no-store" });
      res.end(await readFile(f));
      return;
    }
    if (req.method === "GET" && url.pathname === "/api/info") {
      return send(res, 200, { host: HOST, library: LIB_ROOT, python: PY, hostOk: existsSync(join(HOST, "CMakeLists.txt")) });
    }
    if (req.method === "POST" && url.pathname.startsWith("/api/")) {
      const b = await readBody(req);
      let r;
      if (url.pathname === "/api/scan") {
        // 只算不写：检查、候选、设备目录、diff 预览
        r = await integrate(["--plan", ...commonArgs(b)], b.selection || null);
      } else if (url.pathname === "/api/apply") {
        if (!b.selection) return send(res, 400, { ok: false, fatal: "缺少 selection" });
        const extra = [...commonArgs(b)];
        if (b.overwrite_edited) extra.push("--overwrite-edited");
        r = await integrate(extra, b.selection);
      } else if (url.pathname === "/api/fix-ioc") {
        const ids = Array.isArray(b.ids) && b.ids.length ? b.ids.join(",") : "all";
        const extra = ["--fix-ioc", ids];
        if (b.regenerate) extra.push("--regenerate");
        if (b.cubemx) extra.push("--cubemx", String(b.cubemx));
        if (b.plan) extra.push("--plan");
        if (b.ioc) extra.push("--ioc", String(b.ioc));
        r = await integrate(extra, b.selection || null);
      } else {
        return send(res, 404, { ok: false, fatal: "没有这个接口" });
      }
      return send(res, r.status, r.body);
    }
    send(res, 404, { ok: false, fatal: "没有这个页面" });
  } catch (e) {
    send(res, 500, { ok: false, fatal: String(e && e.message ? e.message : e) });
  }
});

server.listen(PORT, "127.0.0.1", () => {
  console.log(`ICBSP 接入帮助：http://127.0.0.1:${PORT}/`);
  console.log(`宿主工程：${HOST}`);
  console.log(PY.ok ? `Python：${PY.cmd.join(" ")}（${PY.version}）` : `【注意】${PY.error}`);
});
