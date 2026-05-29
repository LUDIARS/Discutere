/**
 * persona-engine 動作 demo (Phase 0)。
 *
 *   npm run persona-demo                   # MockLLM で安全実行 (API 不要)
 *   npm run persona-demo -- --real-llm     # ANTHROPIC_API_KEY を使う実 LLM
 *
 * 1) 一時 DB を tmp に作る (本流 data/discatier.kuzu は触らない)
 * 2) Discatier Core を起動 + design gap + session を作る
 * 3) persona-engine を起動 (推進派 advocate に "GapDetected" event を流す)
 * 4) advocate が hypothesis を提案 (real-llm 時は LLM 出力次第)
 * 5) 結果を JSON で表示
 */

import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import { createCore } from "../src/core/index.js";
import { createDiscatierContextProvider } from "../src/discatier-engine-adapter/index.js";
import {
  AnthropicSdkClient,
  createPersonaEngine,
  MockLLMClient,
  type LLMClient,
} from "../src/persona-engine/index.js";

async function main(): Promise<void> {
  const useRealLlm = process.argv.includes("--real-llm");

  const workDir = path.resolve(".tmp/persona-demo");
  fs.rmSync(workDir, { recursive: true, force: true });
  fs.mkdirSync(workDir, { recursive: true });

  const dbPath = path.join(workDir, "discatier.kuzu");
  const eventPath = path.join(workDir, "events.jsonl");
  const peDbPath = path.join(workDir, "persona-engine.db");

  // Discatier Core
  const core = createCore(dbPath, eventPath);
  const workspaceId = "knowledge";

  const gapId = core.repos.designGap.create({
    workspaceId,
    title: "オンボーディング離脱",
    description: "新規プレイヤーが初回 3 分で 40% 離脱",
    status: "open",
  });
  const sessionId = core.repos.session.create({
    workspaceId,
    title: "design discussion",
    startedAt: Date.now(),
  });

  // persona-engine
  const peDb = new Database(peDbPath);

  const llm: LLMClient = useRealLlm
    ? new AnthropicSdkClient()
    : new MockLLMClient([
        () =>
          '{"action":"propose_hypothesis","statement":"チュートリアル選択肢を 2 つに絞る","addresses_gap_id":"' +
          gapId +
          '","reasoning":"離脱の主因は選択肢多すぎ"}',
      ]);

  const adapter = createDiscatierContextProvider(core, {
    defaultSessionId: sessionId,
  });

  const engine = createPersonaEngine({
    db: peDb,
    llm,
    contextProvider: adapter,
    workspaceId,
    enableLlm: true,
    // tick は使わずイベントだけで駆動
    tickMs: 60_000,
  });

  console.log(JSON.stringify({
    step: "setup",
    llm: useRealLlm ? "AnthropicSdkClient" : "MockLLMClient",
    gapId,
    sessionId,
    personas: engine.personas.list().length,
    rules: engine.rules.list().length,
  }, null, 2));

  // GapDetected を流す → propose-on-gap (target: advocate) が動く
  await engine.fireEvent({ kind: "GapDetected", sessionId });

  const hypotheses = core.client.raw
    .prepare(
      "SELECT id, statement, design_gap_id, status FROM hypotheses WHERE workspace_id = ?"
    )
    .all(workspaceId);
  const recentLogs = engine.rules.recentLogs(8);

  console.log(JSON.stringify({
    step: "result",
    hypotheses,
    rule_log_tail: recentLogs.map((l) => ({
      ts: l.ts,
      rule: l.rule_id,
      action: l.action,
      actor: l.actor,
      detail: l.detail,
    })),
  }, null, 2));

  engine.stop();
  peDb.close();
  core.close();
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
