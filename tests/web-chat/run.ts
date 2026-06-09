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
import {
  ensureWebSession,
  sanitizeRoomId,
  webDiscussionExists,
  webScene,
} from "../../src/web-chat/session.js";
import { readRoomTranscript } from "../../src/web-chat/transcript.js";

const workDir = path.resolve(".tmp/web-chat");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const peDb = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(peDb);

const ws = "knowledge";
const room = "lobby";

// ─── sanitizeRoomId ──────────────────────────────────────────
assert.equal(sanitizeRoomId(" my room!! "), "myroom", "記号と空白は除去");
assert.equal(sanitizeRoomId(""), "lobby", "空は lobby");
assert.equal(sanitizeRoomId("a".repeat(50)).length, 32, "32 文字に切り詰め");
console.log("ok sanitizeRoomId");

// ─── ensureWebSession は冪等 ─────────────────────────────────
const inbox = ensureWebSession(core, ws, room);
assert.equal(ensureWebSession(core, ws, room), inbox, "同 room は同 session");
const inboxScene = core.client.raw
  .prepare("SELECT scene FROM sessions WHERE id = ?")
  .get(inbox) as { scene: string };
assert.equal(inboxScene.scene, webScene(room), "scene=web:<room>");
console.log("ok ensureWebSession");

// 人間の発話を入口 session に記録 (POST /api/chat 相当)。
const humanUttId = core.repos.utterance.create({
  workspaceId: ws,
  sessionId: inbox,
  speakerId: "太郎",
  rawContent: "ローグライトの面白さの核は何か",
  postedAt: Date.now(),
});

// ─── event-bridge: web scene の議論 session 継承 ─────────────
const llm = new MockLLMClient([
  () => JSON.stringify({ action: "propose_hypothesis", statement: "web proposal", reasoning: "r" }),
]);
const logger: Logger = { debug: () => {}, info: () => {}, warn: () => {}, error: () => {} };
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
});
peDb.prepare("UPDATE rules SET cooldown_sec = 0").run();
const bridge = createEventBridge(core, engine, { workspaceId: ws, pollMs: 60_000 });

try {
  // 分類器 starter 相当: gap を作り evidence に web 由来 (guildId="web") を載せる。
  const gapId = core.repos.designGap.create({
    workspaceId: ws,
    title: "ローグライトの面白さの核",
    description: "Web チャットから自動検出",
    status: "open",
  });
  core.client.raw
    .prepare("UPDATE design_gaps SET evidence_json = ?, updated_at = ? WHERE id = ?")
    .run(
      JSON.stringify({
        source: "discord:auto-classifier",
        category: "game_design_question",
        utteranceIds: [humanUttId],
        guildId: "web",
        channelId: room,
        sessionId: inbox,
        authorId: "太郎",
      }),
      Date.now(),
      gapId
    );

  const dispatched = await bridge.pollOnce();
  assert.ok(dispatched >= 1, "DesignGapDetected が dispatch される");

  const gapSession = core.client.raw
    .prepare("SELECT id, scene FROM sessions WHERE workspace_id = ? AND title = ?")
    .get(ws, `discussion-of-gap:${gapId}`) as { id: string; scene: string };
  assert.equal(gapSession.scene, webScene(room), "議論 session が web scene を継承");
  console.log("ok web gap session inherits web scene");

  assert.equal(webDiscussionExists(core, ws, room), true, "open 議論を検知");
  console.log("ok webDiscussionExists");

  // persona の発話 + 人間ミラー (event-bridge handleHumanUtterance 相当) を議論 session に積む。
  core.repos.utterance.create({
    workspaceId: ws,
    sessionId: gapSession.id,
    speakerId: "persona:advocate",
    rawContent: "成長の手応えとビルドの自由度が核だ",
    postedAt: Date.now() + 10,
  });
  core.repos.utterance.create({
    workspaceId: ws,
    sessionId: gapSession.id,
    speakerId: "太郎", // 人間発話のミラー (重複の元)
    rawContent: "ローグライトの面白さの核は何か",
    postedAt: Date.now() + 20,
  });

  // ─── readRoomTranscript: 重複除去 + 役割 + 並び ───────────
  const msgs = readRoomTranscript(core, ws, room, {
    resolveName: (pid) => (pid === "advocate" ? "擁護者" : pid),
  });
  const humans = msgs.filter((m) => m.role === "human");
  const personas = msgs.filter((m) => m.role === "persona");
  assert.equal(humans.length, 1, "人間発話はミラー込みでも 1 回だけ");
  assert.equal(humans[0].speaker, "太郎", "人間は表示名 author");
  assert.equal(personas.length, 1, "persona 発話が出る");
  assert.equal(personas[0].speaker, "擁護者", "persona は display_name 解決");
  // 時系列: 人間(入口) → persona の順
  assert.ok(
    msgs.findIndex((m) => m.role === "human") < msgs.findIndex((m) => m.role === "persona"),
    "古い順"
  );
  console.log("ok readRoomTranscript dedup + role + order");

  // since カーソル: persona 発話の時刻以降だけ取得
  const cutoff = personas[0].postedAt;
  const incr = readRoomTranscript(core, ws, room, { sinceTs: cutoff });
  assert.ok(
    incr.every((m) => m.postedAt >= cutoff),
    "since 以降のみ"
  );
  assert.ok(
    incr.some((m) => m.role === "persona"),
    "境界 (>=) で persona を取りこぼさない"
  );
  console.log("ok readRoomTranscript since cursor");
} finally {
  bridge.stop();
  engine.stop();
  peDb.close();
  core.close();
}

console.log("web-chat tests: all passed");
