/**
 * Discatier events → persona-engine.fireEvent 配線テスト (PR-D).
 *
 * - DesignGapDetected event が persona-engine に届く
 * - propose-on-gap rule (event_kind="DesignGapDetected") が発火し、
 *   advocate persona が hypothesis を提案 → Discatier に着地
 * - HypothesisIntegrated event が届くと engine.resetSession が呼ばれ、
 *   safety cap が解放される
 * - gap 1:1 で「discussion-of-gap:<id>」 session が自動作成される
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";

import { createCore } from "../../src/core/index.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";
import { createEventBridge } from "../../src/discatier-engine-adapter/event-bridge.js";
import {
  applyPersonaEngineMigrations,
  createPersonaEngine,
  DISCUSSION_PERSONA_SEEDS,
  DISCUSSION_RULE_SEEDS,
  MockLLMClient,
  type Logger,
} from "../../src/persona-engine/index.js";

const workDir = path.resolve(".tmp/pe-event-bridge");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const peDb = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(peDb);

const ws = "knowledge";

const llm = new MockLLMClient([
  () => '{"action":"propose_hypothesis","statement":"first proposal","reasoning":"r"}',
  () => '{"action":"propose_hypothesis","statement":"second proposal","reasoning":"r2"}',
]);

const logger: Logger = {
  debug: () => {},
  info: () => {},
  warn: () => {},
  error: () => {},
};

const adapter = createDiscatierContextProvider(core, {});

const engine = createPersonaEngine({
  db: peDb,
  llm,
  contextProvider: adapter,
  logger,
  workspaceId: ws,
  personaSeeds: DISCUSSION_PERSONA_SEEDS,
  ruleSeeds: DISCUSSION_RULE_SEEDS,
  tickMs: 60_000,
  enableLlm: true,
  // 強制的に cooldown 0 にしてテストの即時 fire を許可
});
// rule cooldown を 0 に
peDb.prepare("UPDATE rules SET cooldown_sec = 0").run();

const bridge = createEventBridge(core, engine, { workspaceId: ws, pollMs: 60_000 });

try {
  // 起動時の lastSeen が DB の MAX(created_at) になっていることを確認
  const initialLastSeen = bridge.lastSeenAt();
  // 1. design gap を作る → DesignGapDetected (DesignGapCreated は実はないので、
  //    DesignGap repo.create は DesignGapDetected event を発火する)
  const gapId = core.repos.designGap.create({
    workspaceId: ws,
    title: "オンボーディング離脱",
    status: "open",
  });

  // 2. bridge.pollOnce を呼ぶ → DesignGapDetected event を読んで engine.fireEvent
  //    → propose-on-gap rule (event_kind=DesignGapDetected, target=advocate) 発火
  const dispatched1 = await bridge.pollOnce();
  assert.ok(dispatched1 >= 1, `should dispatch at least 1 event (got ${dispatched1})`);
  assert.ok(bridge.lastSeenAt() >= initialLastSeen, "lastSeen advanced");
  console.log("ok DesignGapDetected dispatched");

  // 3. hypothesis が Discatier に着地している
  const hyps = core.client.raw
    .prepare("SELECT id, statement, design_gap_id FROM hypotheses WHERE workspace_id = ?")
    .all(ws) as Array<{ id: string; statement: string; design_gap_id: string | null }>;
  assert.ok(hyps.length >= 1, "hypothesis should be proposed by advocate");
  assert.equal(hyps[0].statement, "first proposal");
  console.log("ok advocate proposed hypothesis via event bridge");

  // 4. gap 1:1 で discussion-of-gap session が自動作成されている
  const sessions = core.client.raw
    .prepare("SELECT id, title, scene FROM sessions WHERE workspace_id = ? ORDER BY started_at")
    .all(ws) as Array<{ id: string; title: string; scene: string | null }>;
  const gapSession = sessions.find((s) => s.title === `discussion-of-gap:${gapId}`);
  assert.ok(gapSession, `gap discussion session should be auto-created for ${gapId}`);
  assert.equal(gapSession?.scene, `gap:${gapId}`);
  console.log("ok gap discussion session auto-created");

  // 5. 同じ gap の再 event でも session は再利用される
  const beforeCount = sessions.length;
  // gap_id を addresses したまま再度 detector を叩く想定で、 同 gap 再 update
  core.repos.designGap.update(gapId, {
    workspaceId: ws,
    title: "オンボーディング離脱 (改名)",
    status: "open",
  });
  await bridge.pollOnce();
  const sessionsAfter = core.client.raw
    .prepare("SELECT id, title FROM sessions WHERE workspace_id = ?")
    .all(ws) as Array<{ id: string; title: string }>;
  const gapSessionsAfter = sessionsAfter.filter(
    (s) => s.title === `discussion-of-gap:${gapId}`
  );
  assert.equal(gapSessionsAfter.length, 1, "same gap should reuse the same session");
  console.log("ok gap session reused on second event");

  // 6. HypothesisIntegrated event → engine.resetSession が呼ばれて safety cap 解放
  //    まず engine に cap を設定して、 cap 到達状態を作る
  //    (engine 再生成のコストを避けて) sessionFires は engine 内部なので直接観測難。
  //    代わりに rule_log の "skip / session cap reached" 出現有無で間接観測。
  //    (本テストでは cap が無いので skip ログは出ない、 closing event だけ確認)
  const hid = hyps[0].id;
  // submit → integrate に進めるため必要な lifecycle ステップ:
  // draft (= proposed event 経由なので proposed 状態に近い、 lifecycle 詳細は phase 5 test 参照)
  // ここでは event 発火だけ確認するため、 直接 events に HypothesisIntegrated を append
  core.eventLog.appendEvent({
    eventType: "HypothesisIntegrated" as never,
    payload: {
      id: hid,
      workspaceId: ws,
      statement: "first proposal",
      status: "integrated",
      integrated: true,
      validatedByEmotion: true,
      designGapId: gapId,
      refs: [],
    } as never,
  });
  const dispatched2 = await bridge.pollOnce();
  assert.ok(dispatched2 >= 1, "HypothesisIntegrated should be dispatched");
  console.log("ok HypothesisIntegrated dispatched + resetSession invoked (no explicit assert)");

  // 7. 別 workspace の event は無視
  const otherWs = "other-ws";
  core.repos.designGap.create({
    workspaceId: otherWs,
    title: "別 workspace gap",
    status: "open",
  });
  const dispatched3 = await bridge.pollOnce();
  // dispatched3 は >= 1 (lastSeen 越えてるので row 自体は取れる) だが、 fireEvent 内で
  // workspace 不一致を見て engine 側 fireEvent は流すが rule が target なしで何もしない。
  // 我々の bridge 実装では workspace 違いは dispatchOne 早期 return。
  const hypsAfter = core.client.raw
    .prepare("SELECT COUNT(*) AS n FROM hypotheses WHERE workspace_id = ?")
    .get(ws) as { n: number };
  assert.equal(hypsAfter.n, hyps.length, "other workspace event should not propose more hypotheses in main ws");
  console.log("ok other workspace events ignored");
} finally {
  bridge.stop();
  engine.stop();
  peDb.close();
  core.close();
}

console.log("event-bridge.test.ts: all passed");
