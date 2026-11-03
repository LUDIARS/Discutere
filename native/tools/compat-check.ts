// Node 版 (src/index.ts) と C++ 版 (discutere-server) の互換性検査 (job cpp-node-20261030, w6-main)。
//
// 同じ合成データ (または --data で渡したデータのコピー) と同じ env で両方を別ポートに起動し、
// 副作用の無い GET 経路に同じ要求を投げて status・Content-Type・本文 (JSON はキー順込み) を比べ、差分を表で出す。
// 時刻 (ISO / epoch ms)・UUID・ポート番号は正規化してから比べる。
//
// 秘密は使わない (偽の値)。外部サービスに繋がない: Discord token 無し・Slack / backup / kg-sync / cost-relay / embedding 無効、
// LLM は claude-cli (GET では呼ばれない)。--config で本番相当の設定を渡したときも、これらの項目は消してから使う。
//
// 使い方 (リポのルートで):
//   npx tsx native/tools/compat-check.ts [--cpp-bin <path>] [--data <dataDir>] [--config <config.json>]
//                                        [--out <report.md>] [--keep] [--verbose]
//   --cpp-bin  C++ 版の実行ファイル (既定 env DI_SERVER_BIN、無ければ /home/user/build-w6-main/bin/discutere-server)
//   --data     サーバの cwd に data/ として複写するディレクトリ (既定: 空 = 合成データだけ)
//   --config   discutere.config.json (秘密・外部接続の項目は消して使う)
// 終了コード: 差分なし 0 / 差分あり 1 / 起動失敗 2。
import { spawn, type ChildProcess } from "node:child_process";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createServer } from "node:net";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

type Args = { cppBin: string; data?: string; config?: string; out?: string; keep: boolean; verbose: boolean };

function parseArgs(argv: string[]): Args {
  const a: Args = {
    cppBin: process.env.DI_SERVER_BIN ?? "/home/user/build-w6-main/bin/discutere-server",
    keep: false,
    verbose: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const v = () => {
      const x = argv[++i];
      if (x === undefined) throw new Error(`${k} needs a value`);
      return x;
    };
    if (k === "--cpp-bin") a.cppBin = resolve(v());
    else if (k === "--data") a.data = resolve(v());
    else if (k === "--config") a.config = resolve(v());
    else if (k === "--out") a.out = resolve(v());
    else if (k === "--keep") a.keep = true;
    else if (k === "--verbose") a.verbose = true;
    else throw new Error(`unknown argument: ${k}`);
  }
  return a;
}

const repoRoot = resolve(new URL("../..", import.meta.url).pathname);

/** 空きポート (port=0 で bind して閉じる)。 */
function freePort(): Promise<number> {
  return new Promise((ok, ng) => {
    const s = createServer();
    s.once("error", ng);
    s.listen(0, "127.0.0.1", () => {
      const addr = s.address();
      const port = typeof addr === "object" && addr ? addr.port : 0;
      s.close(() => ok(port));
    });
  });
}

/** 本番相当の config から秘密と外部接続を消す。 */
function sanitizeConfig(raw: unknown): Record<string, unknown> {
  const c = (raw && typeof raw === "object" && !Array.isArray(raw) ? structuredClone(raw) : {}) as Record<string, any>;
  if (c.discord && typeof c.discord === "object") {
    delete c.discord.botToken;
    delete c.discord.applicationId;
  }
  delete c.slack;
  delete c.backup;
  delete c.kgSync;
  delete c.cost;
  delete c.externalDiscussion;
  if (c.llm && typeof c.llm === "object") {
    delete c.llm.anthropicApiKey;
    c.llm.backend = "claude-cli";
    if (c.llm.local && typeof c.llm.local === "object") delete c.llm.local.apiKey;
  }
  if (c.embedding && typeof c.embedding === "object") {
    c.embedding.enabled = false;
    delete c.embedding.apiKey;
  }
  if (c.workerPool && typeof c.workerPool === "object") c.workerPool.enabled = false;
  return c;
}

/** 両サーバに同じ env (親の env は PATH だけ引き継ぐ)。 */
function serverEnv(dir: string, port: number, configPath: string): NodeJS.ProcessEnv {
  return {
    PATH: process.env.PATH ?? "/usr/bin:/bin",
    HOME: dir,
    NODE_ENV: "test",
    TZ: "UTC",
    BACKEND_HOST: "127.0.0.1",
    BACKEND_PORT: String(port),
    FRONTEND_URL: "http://localhost:5174",
    DISCUTERE_CONFIG: configPath,
    VESTIGIUM_LOGS_DIR: join(dir, "logs"),
    LLM_BACKEND: "claude-cli",
    DISCUTERE_CLASSIFIER_BACKEND: "off",
    DISCUTERE_SLACK_ENABLED: "false",
    DISCUTERE_BACKUP_ENABLED: "false",
    DISCUTERE_KG_SYNC_AUTO: "false",
    DISCUTERE_COST_RELAY_ENABLED: "false",
    DISCUTERE_EMBEDDING_ENABLED: "false",
    DISCUTERE_WORKER_POOL_ENABLED: "false",
  };
}

type Server = { name: string; port: number; dir: string; child: ChildProcess; log: string[] };

function startServer(name: string, argv: string[], dir: string, port: number, configPath: string): Server {
  const child = spawn(argv[0], argv.slice(1), { cwd: dir, env: serverEnv(dir, port, configPath), stdio: ["ignore", "pipe", "pipe"] });
  const s: Server = { name, port, dir, child, log: [] };
  const push = (b: Buffer) => {
    s.log.push(b.toString("utf8"));
    if (s.log.length > 2000) s.log.shift();
  };
  child.stdout?.on("data", push);
  child.stderr?.on("data", push);
  return s;
}

async function waitHealthy(s: Server, timeoutMs: number): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (s.child.exitCode !== null) return false;
    try {
      const r = await fetch(`http://127.0.0.1:${s.port}/health`, { signal: AbortSignal.timeout(2000) });
      if (r.status === 200) return true;
    } catch {
      /* まだ */
    }
    await new Promise((r) => setTimeout(r, 300));
  }
  return false;
}

async function stopServer(s: Server): Promise<number | null> {
  if (s.child.exitCode !== null) return s.child.exitCode;
  const done = new Promise<number | null>((ok) => s.child.once("exit", (code) => ok(code)));
  s.child.kill("SIGTERM");
  const t = setTimeout(() => s.child.kill("SIGKILL"), 25_000);
  const code = await done;
  clearTimeout(t);
  return code;
}

// ─── 比べる GET 経路 (index.ts と各 routes から読み取った、副作用の無い GET) ───
// 除外: /api/spec/github/auth (gh CLI を起動し GitHub に問い合わせる)。
const ROUTES: { path: string; headers?: Record<string, string> }[] = [
  { path: "/health" },
  { path: "/" },
  { path: "/guide" },
  { path: "/flow" },
  { path: "/chat" },
  { path: "/learning" },
  { path: "/learning/data" },
  { path: "/learning/sources" },
  { path: "/learning/conclusions" },
  { path: "/learning/conclusion?gap=no-such-gap" },
  { path: "/learning/conclusion/export?gap=no-such-gap" },
  { path: "/learning/gap?id=no-such-gap" },
  { path: "/learning/gap/detail?id=no-such-gap" },
  { path: "/ludus/economy-graph/no-such-game" },
  { path: "/api/ludus/economy-graph/no-such-game" },
  { path: "/api/ludus/economy-graphs" },
  { path: "/api/chat/sessions" },
  { path: "/api/chat/sessions?state=open&limit=5&offset=0" },
  { path: "/api/chat/room-a/messages" },
  { path: "/api/chat/room-a/messages?since=0" },
  { path: "/api/chat/no-such-room/messages" },
  { path: "/api/flow/sessions" },
  { path: "/api/flow/sessions?state=draft&limit=10&offset=0" },
  { path: "/api/flow/sessions?state=bogus" },
  { path: "/api/flow/no-such-session/status" },
  { path: "/api/flow/no-such-session/paper" },
  { path: "/api/admin/status" },
  { path: "/api/admin/status", headers: { "X-User-Id": "u-1" } },
  { path: "/api/admin/status", headers: { "X-User-Id": "u-1", "X-User-Role": "admin" } },
  { path: "/api/admin/dashboard" },
  { path: "/api/admin/errors" },
  { path: "/api/admin/learning/topics" },
  { path: "/api/admin/metrics/personas" },
  { path: "/api/admin/metrics/rules" },
  { path: "/api/admin/rules" },
  { path: "/api/admin/rules/no-such-rule/logs" },
  { path: "/api/admin/queue" },
  { path: "/api/admin/consensus" },
  { path: "/api/admin/consensus/gaps" },
  { path: "/api/admin/consensus/no-such-gap" },
  { path: "/api/admin/consensus/no-such-gap/personas" },
  { path: "/api/admin/noise" },
  { path: "/api/admin/noise/groups" },
  { path: "/api/admin/noise/utterances" },
  { path: "/api/admin/datasources" },
  { path: "/api/admin/datasources/youtube/utterances" },
  { path: "/api/admin/datasources/youtube/utterances?limit=abc" },
  { path: "/api/admin/personas" },
  { path: "/api/admin/personas/data" },
  { path: "/api/admin/tuning" },
  { path: "/api/admin/tuning/data" },
  { path: "/api/admin/blackbox/rules" },
  { path: "/api/admin/blackbox/decisions" },
  { path: "/api/admin/conclusion-publication" },
  { path: "/api/admin/conclusion-publication/editor" },
  { path: "/api/worker-pool" },
  { path: "/api/worker-pool/status" },
  { path: "/api/kg/migrations" },
  { path: "/api/integrations/glab/review-trends" },
  { path: "/api/integrations/glab/conclusions" },
  { path: "/api/persona-bridge/utterances" },
  { path: "/no/such/route" },
  { path: "/api/no/such/route" },
];

/** 時刻・UUID・ポートを正規化する。 */
function normalizeText(s: string, ports: number[]): string {
  let t = s;
  for (const p of ports) t = t.split(`:${p}`).join(":<port>");
  t = t.replace(/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/gi, "<uuid>");
  t = t.replace(/\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z/g, "<iso>");
  t = t.replace(/\b1[6-9]\d{11}\b/g, "<ms>");
  return t;
}

function normalizeJson(v: unknown, ports: number[]): unknown {
  if (Array.isArray(v)) return v.map((x) => normalizeJson(x, ports));
  if (v && typeof v === "object") {
    const out: Record<string, unknown> = {};
    for (const [k, x] of Object.entries(v as Record<string, unknown>)) out[k] = normalizeJson(x, ports);
    return out;
  }
  if (typeof v === "number" && Number.isInteger(v) && v >= 1.6e12 && v < 2e12) return "<ms>";
  if (typeof v === "string") return normalizeText(v, ports);
  return v;
}

type Observed = { status: number; contentType: string; body: string; isJson: boolean };

async function observe(port: number, path: string, headers: Record<string, string> | undefined, ports: number[]): Promise<Observed> {
  try {
    const r = await fetch(`http://127.0.0.1:${port}${path}`, { headers, redirect: "manual", signal: AbortSignal.timeout(30_000) });
    const contentType = r.headers.get("content-type") ?? "";
    const text = await r.text();
    if (contentType.startsWith("application/json")) {
      try {
        return { status: r.status, contentType, body: JSON.stringify(normalizeJson(JSON.parse(text), ports)), isJson: true };
      } catch {
        /* 壊れた JSON はテキストとして比べる */
      }
    }
    return { status: r.status, contentType, body: normalizeText(text, ports), isJson: false };
  } catch (e) {
    return { status: 0, contentType: "", body: `fetch failed: ${(e as Error).message}`, isJson: false };
  }
}

function firstDiff(a: string, b: string): string {
  let i = 0;
  while (i < a.length && i < b.length && a[i] === b[i]) i++;
  const ctx = (s: string) => JSON.stringify(s.slice(Math.max(0, i - 30), i + 50));
  return `@${i}: node=${ctx(a)} cpp=${ctx(b)}`;
}

/** 両方に同じ合成データを API で入れる (LLM を呼ばない経路だけ)。 */
async function seed(ports: number[]): Promise<void> {
  for (const port of ports) {
    for (const [room, text] of [
      ["room-a", "合成の発話: 序盤の育成が単調だと感じる"],
      ["room-a", "合成の発話: 報酬の間隔をもう少し短くしてほしい"],
      ["room-b", "合成の発話: 対戦のマッチングが偏っている"],
    ]) {
      await fetch(`http://127.0.0.1:${port}/api/chat/${room}/messages`, {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ text, author: "合成ユーザー" }),
      }).catch(() => undefined);
    }
  }
  await new Promise((r) => setTimeout(r, 1500));
}

async function main(): Promise<number> {
  const args = parseArgs(process.argv.slice(2));
  if (!existsSync(args.cppBin)) {
    console.error(`C++ binary not found: ${args.cppBin}`);
    return 2;
  }
  const root = mkdtempSync(join(tmpdir(), "di-compat-"));
  const dirs = { node: join(root, "node"), cpp: join(root, "cpp") };
  const servers: Server[] = [];
  try {
    for (const d of Object.values(dirs)) {
      mkdirSync(d, { recursive: true });
      if (args.data) cpSync(args.data, join(d, "data"), { recursive: true });
      else mkdirSync(join(d, "data"), { recursive: true });
    }
    const configPath = join(root, "compat.config.json");
    if (args.config) writeFileSync(configPath, JSON.stringify(sanitizeConfig(JSON.parse(readFileSync(args.config, "utf8"))), null, 2));
    // --config が無ければ存在しないパス (= env と既定だけ)。

    const [nodePort, cppPort] = [await freePort(), await freePort()];
    const tsx = join(repoRoot, "node_modules", "tsx", "dist", "esm", "index.mjs");
    servers.push(startServer("node", [process.execPath, "--import", tsx, join(repoRoot, "src", "index.ts")], dirs.node, nodePort, configPath));
    servers.push(startServer("cpp", [args.cppBin], dirs.cpp, cppPort, configPath));
    for (const s of servers) {
      if (!(await waitHealthy(s, 120_000))) {
        console.error(`${s.name} did not become healthy. log tail:\n${s.log.join("").slice(-4000)}`);
        return 2;
      }
    }
    const ports = [nodePort, cppPort];
    await seed(ports);

    const rows: string[] = [];
    let diffs = 0;
    for (const r of ROUTES) {
      const [n, c] = [await observe(nodePort, r.path, r.headers, ports), await observe(cppPort, r.path, r.headers, ports)];
      const statusOk = n.status === c.status;
      const ctOk = n.contentType === c.contentType;
      const bodyOk = n.body === c.body;
      const ok = statusOk && ctOk && bodyOk;
      if (!ok) diffs++;
      const label = r.headers ? `${r.path} (${Object.entries(r.headers).map(([k, v]) => `${k}: ${v}`).join(", ")})` : r.path;
      const note = ok ? "" : [!statusOk ? "status" : "", !ctOk ? `ct node=${n.contentType} cpp=${c.contentType}` : "", !bodyOk ? `body ${firstDiff(n.body, c.body)}` : ""]
        .filter(Boolean)
        .join("; ");
      rows.push(`| ${ok ? "OK" : "DIFF"} | \`${label}\` | ${n.status} | ${c.status} | ${n.isJson ? "json" : n.contentType.split(";")[0]} | ${note.replace(/\|/g, "\\|")} |`);
      if (args.verbose && !ok) console.error(`--- ${label}\nnode: ${n.body.slice(0, 2000)}\ncpp:  ${c.body.slice(0, 2000)}`);
    }
    const report = [
      `# compat-check (${new Date().toISOString()})`,
      "",
      `経路 ${ROUTES.length} 件 / 差分 ${diffs} 件 (data=${args.data ?? "合成 (空 + チャット投稿 3 件)"}, config=${args.config ? "sanitized" : "なし"})`,
      "",
      "| 結果 | 経路 | Node | C++ | 種類 | 差分 |",
      "|---|---|---|---|---|---|",
      ...rows,
      "",
    ].join("\n");
    console.log(report);
    if (args.out) writeFileSync(args.out, report);
    const codes = await Promise.all(servers.map(stopServer));
    servers.length = 0;
    console.log(`exit codes after SIGTERM: node=${codes[0]} cpp=${codes[1]}`);
    return diffs === 0 ? 0 : 1;
  } finally {
    await Promise.all(servers.map(stopServer));
    if (!args.keep) rmSync(root, { recursive: true, force: true });
    else console.log(`kept: ${root}`);
  }
}

main().then(
  (code) => process.exit(code),
  (e) => {
    console.error(e);
    process.exit(2);
  },
);
