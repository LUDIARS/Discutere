import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { createEventBridge } from "../../src/discatier-engine-adapter/event-bridge.js";
import type { PersonaEngineHandle } from "../../src/persona-engine/index.js";

// event-bridge の「人間発言 → 議論 session へミラー + HumanUtterance 発火」 を検証。
// engine は fireEvent を捕捉するスタブ (persona-engine 本体は別テストで担保)。

const workDir = path.resolve(".tmp/pe-human-utterance");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const ws = "knowledge";
const guildId = "g1";
const channelId = "c1";
const scene = `discord:${guildId}/${channelId}`;

// 平文取り込み用の discord-session (人間の発言先)
const discordSessionId = core.repos.session.create({
  workspaceId: ws,
  title: `discord-session:${guildId}:${channelId}`,
  startedAt: Date.now(),
  scene,
});

// 同チャンネルで進行中の議論 (open gap + discussion-of-gap session、 scene 共有)
const gapId = core.repos.designGap.create({ workspaceId: ws, title: "面白さの本質", status: "open" });
const discussionSessionId = core.repos.session.create({
  workspaceId: ws,
  title: `discussion-of-gap:${gapId}`,
  startedAt: Date.now(),
  scene,
});

const fired: Array<{ kind: string; sessionId: string | null }> = [];
const stubEngine = {
  fireEvent: async (e: { kind: string; sessionId?: string | null }) => {
    fired.push({ kind: e.kind, sessionId: e.sessionId ?? null });
  },
  resetSession: () => {},
} as unknown as PersonaEngineHandle;

const bridge = createEventBridge(core, stubEngine, { workspaceId: ws, pollMs: 60_000 });

// bridge は構築時の最新 event を起点にする。 同一 ms 衝突で初回 utterance を
// 取りこぼさないよう、 起点確定後にわずかに時刻を進める。
const tick = () => new Promise((r) => setTimeout(r, 5));

try {
  await tick();
  // 1) 人間が discord-session に発言 → ミラー + HumanUtterance
  core.repos.utterance.create({
    workspaceId: ws,
    sessionId: discordSessionId,
    speakerId: "u1", // 人間 (persona:/ext: でない)
    rawContent: "もっと否定的な意見も聞きたい",
    postedAt: Date.now(),
  });
  await bridge.pollOnce();

  const human = fired.filter((f) => f.kind === "HumanUtterance");
  assert.equal(human.length, 1, "人間発言で HumanUtterance が 1 回発火");
  assert.equal(human[0].sessionId, discussionSessionId, "発火先は進行中の議論 session");

  const mirrored = core.client.raw
    .prepare("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'u1'")
    .all(discussionSessionId) as Array<{ raw_content: string }>;
  assert.equal(mirrored.length, 1, "人間発言が議論 session にミラーされる");
  assert.equal(mirrored[0].raw_content, "もっと否定的な意見も聞きたい");
  console.log("ok human utterance mirrors + fires HumanUtterance");

  // 2) ミラー (= discussion-of-gap 内の人間発言) は再発火しない (ループ防止)
  fired.length = 0;
  await bridge.pollOnce(); // ミラーで生まれた UtteranceCreated を処理
  assert.equal(
    fired.filter((f) => f.kind === "HumanUtterance").length,
    0,
    "議論 session 内の人間発言は HumanUtterance を再発火しない"
  );
  console.log("ok mirrored human utterance does not re-fire (loop guard)");

  // 3) gap が closed なら 人間発言を拾わない (収束後の沈黙)
  core.repos.designGap.update(gapId, { workspaceId: ws, title: "面白さの本質", status: "closed" });
  await tick();
  fired.length = 0;
  core.repos.utterance.create({
    workspaceId: ws,
    sessionId: discordSessionId,
    speakerId: "u1",
    rawContent: "まだ話したい",
    postedAt: Date.now(),
  });
  await bridge.pollOnce();
  assert.equal(
    fired.filter((f) => f.kind === "HumanUtterance").length,
    0,
    "closed gap の議論には HumanUtterance を発火しない"
  );
  console.log("ok closed discussion ignores human utterance");
} finally {
  bridge.stop();
  core.close();
}

console.log("human-utterance.test.ts: all passed");
