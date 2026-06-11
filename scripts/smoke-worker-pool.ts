/**
 * P-c smoke: 常駐ワーカー 1 体で register → ターン注入 → callback の往復を実機確認。
 *
 * 稼働中 Discutere(3100) を乱さないよう、独立した callback HTTP サーバ + WorkerPool を
 * 立ててテストする。
 *
 *   npx tsx scripts/smoke-worker-pool.ts
 */

import http from "node:http";
import { join } from "node:path";

import { WorkerPool } from "../src/persona-engine/worker-pool/pool.js";
import type { WorkerConfig } from "../src/persona-engine/worker-pool/types.js";

const PORT = 3398;
const WORKER: WorkerConfig = { id: "con-opus", role: "否定派", provider: "claude", model: "claude-opus-4-8" };

const pool = new WorkerPool(
  {
    enabled: true,
    workspace: "debate",
    callbackBaseUrl: `http://127.0.0.1:${PORT}`,
    workerCwd: join(process.cwd(), "worker-home"),
    injectDelayMs: 2500,
    turnTimeoutMs: 180_000,
    registerTimeoutMs: 120_000,
    registerSettleMs: 4_000,
    turnsDir: "data/worker-turns",
    promptsDir: "data/worker-prompts",
    workers: [WORKER],
  },
  process.cwd()
);

// callback サーバ (Discutere の /internal/worker/* 相当を最小実装)。
const server = http.createServer((req, res) => {
  let body = "";
  req.on("data", (c) => (body += c));
  req.on("end", () => {
    try {
      const j = body ? JSON.parse(body) : {};
      if (req.url === "/internal/worker/register") {
        console.log(`[smoke] register: workerId=${j.workerId} port=${j.lictorPort} lictorPid=${j.lictorPid ?? "(unknown)"}`);
        pool.registerPort(j.workerId, j.lictorPort, typeof j.lictorPid === "number" ? j.lictorPid : undefined);
      } else if (req.url === "/internal/worker/utterance") {
        console.log(`[smoke] utterance callback: reqId=${j.reqId}`);
        pool.onUtterance(j.reqId, j.workerId ?? "", j.text ?? "");
      }
      res.writeHead(200, { "content-type": "application/json" });
      res.end('{"ok":true}');
    } catch (e) {
      res.writeHead(400);
      res.end(String(e));
    }
  });
});

async function main(): Promise<void> {
  await new Promise<void>((r) => server.listen(PORT, "127.0.0.1", r));
  console.log(`[smoke] callback server on http://127.0.0.1:${PORT}`);

  console.log("[smoke] spawning 1 worker (con-opus / opus)...");
  pool.start();

  // register + settle 待ち (isReady = register 受領 + registerSettleMs 経過)
  const regDeadline = Date.now() + 120_000 + 4_000;
  while (!pool.isReady(WORKER.id)) {
    if (Date.now() > regDeadline) throw new Error("worker register+settle timeout");
    await new Promise((r) => setTimeout(r, 1500));
  }
  console.log("[smoke] worker idle-ready (registered + settled), dispatching a turn...");

  const prompt = [
    "## 議論コンテキスト (JSON)",
    "```json",
    JSON.stringify(
      {
        theme: "ローグライクの死亡ペナルティは重い方が面白い",
        utterances: [
          { by: "ユーザA", text: "全ロストするから一手一手に緊張感が出る。軽いと作業ゲーになる。" },
          { by: "ユーザB", text: "ハイリスクだから街に帰れた時の達成感がすごい。" },
        ],
      },
      null,
      2
    ),
    "```",
    "",
    "上記の議論に、あなたの役割で発言を1つ返してください。",
  ].join("\n");

  const text = await pool.dispatch(WORKER.id, { reqId: "smoke-1", system: "", prompt });
  console.log("\n================ WORKER UTTERANCE ================");
  console.log(text);
  console.log("=================================================\n");

  pool.stop();
  server.close();
  console.log("[smoke] done (worker killed).");
  process.exit(0);
}

main().catch((e) => {
  console.error("[smoke] FAILED:", e);
  try {
    pool.stop();
  } catch {
    /* ignore */
  }
  process.exit(1);
});
