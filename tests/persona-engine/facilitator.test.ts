import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import Database from "better-sqlite3";

import { createCore } from "../../src/core/index.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";
import { PersonasRepo } from "../../src/persona-engine/db/personas-repo.js";
import { applyPersonaEngineMigrations } from "../../src/persona-engine/db/migrations.js";
import { createFacilitator, evaluate } from "../../src/persona-engine/facilitator/index.js";
import type { LLMClient } from "../../src/persona-engine/llm/client.js";

// ───────── 1. evaluate (純粋判定) ─────────
const opts = { idleGapMs: 1000 };
// 新発言あり → active + 再 arm
assert.deepEqual(
  evaluate({ utteranceCount: 5, distinctPersonas: 3, lastActivityAt: 0, now: 9999 }, { lastUtteranceCount: 4, armed: false }, opts),
  { mode: "active", state: { lastUtteranceCount: 5, armed: true } }
);
// idle + armed → intervene
assert.equal(
  evaluate({ utteranceCount: 5, distinctPersonas: 3, lastActivityAt: 0, now: 5000 }, { lastUtteranceCount: 5, armed: true }, opts).mode,
  "intervene"
);
// idle だが disarm 済 → wait
assert.equal(
  evaluate({ utteranceCount: 5, distinctPersonas: 3, lastActivityAt: 0, now: 5000 }, { lastUtteranceCount: 5, armed: false }, opts).mode,
  "wait"
);
// まだ idle 未達 → wait
assert.equal(
  evaluate({ utteranceCount: 5, distinctPersonas: 3, lastActivityAt: 4900, now: 5000 }, { lastUtteranceCount: 5, armed: true }, opts).mode,
  "wait"
);
console.log("ok facilitator evaluate");

// ───────── 2. tickOnce (mock LLM で expand / 止揚収束) ─────────
const workDir = path.resolve(".tmp/facilitator");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "kg.kuzu"), path.join(workDir, "events.jsonl"));
const peDb = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(peDb);
const personas = new PersonasRepo(peDb);
const adapter = createDiscatierContextProvider(core);
const ws = "knowledge";
const OLD = Date.now() - 5_000;

function makeDiscussion(
  title: string,
  personaSpeakers: string[],
  scene = "discord:1/2"
): { gapId: string; sessionId: string } {
  const gapId = core.repos.designGap.create({ workspaceId: ws, title });
  const sessionId = core.repos.session.create({
    workspaceId: ws,
    title: `discussion-of-gap:${gapId}`,
    startedAt: OLD,
    scene,
  } as never);
  personaSpeakers.forEach((sp, i) =>
    core.repos.utterance.create({ workspaceId: ws, sessionId, speakerId: sp, rawContent: `発言${i}`, postedAt: OLD })
  );
  return { gapId, sessionId };
}

// scene を分ける: 収束は scene 単位で session を閉じるため、別議論が同 scene を共有すると
// 巻き込まれる (intake + discussion-of-gap が同 scene を共有する実運用を模した仕様)。
const expandCase = makeDiscussion("拡張テスト", ["persona:advocate"], "discord:1/10"); // 止揚まだ → expand
const convergeCase = makeDiscussion("収束テスト", ["persona:advocate"], "discord:1/20"); // 止揚出る → converge

// mock LLM: prompt 種別を本文で判別 (止揚判定 / 拡張 / まとめ)
const mockLlm: LLMClient = {
  async invoke(args) {
    const u = args.prompt;
    if (u.includes("ストック済みの止揚")) {
      // 止揚判定: 「収束テスト」議題でだけ止揚を検出
      const found = u.includes("収束テスト");
      return { ok: true, text: found ? '{ "found": true, "summary": "両論を統合した一段上の結論X" }' : '{ "found": false, "summary": "" }' };
    }
    if (u.includes("到達した止揚")) {
      return { ok: true, text: '{ "summary": "止揚Xを軸にした結論。" }' };
    }
    // 拡張 (新 persona)
    return {
      ok: true,
      text: '{ "name": "コスト懐疑", "display_name": "費田 渋", "description": "費用対効果を問う", "traits": ["慎重"], "speech_style": "淡々", "opening": "本当にそれは時間に見合う?" }',
    };
  },
};

const fac = createFacilitator({
  core,
  llm: mockLlm,
  contextProvider: adapter,
  personas,
  workspaceId: ws,
  logger: { debug() {}, info() {}, warn() {}, error() {} },
  options: { idleGapMs: 1000, maxPersonas: 50, aufhebungTarget: 1 },
});

await fac.tickOnce();

// expandCase: 止揚なし → 新 persona 投入
const expandUtts = core.client.raw
  .prepare("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'")
  .all(expandCase.sessionId) as Array<{ raw_content: string }>;
assert.equal(expandUtts.length, 1, "止揚が無い議論は新 persona で広げる");
console.log("ok facilitator expand (止揚なし)");

// convergeCase: 止揚 1 つ stock → aufhebungTarget(1) 到達 → 収束
const stock = core.client.raw
  .prepare("SELECT summary FROM aufhebung_stock WHERE gap_id = ?")
  .all(convergeCase.gapId) as Array<{ summary: string }>;
assert.equal(stock.length, 1, "止揚が裏レイヤにストックされる");
assert.ok(stock[0].summary.includes("統合"), "止揚の要旨");
const convUtt = core.client.raw
  .prepare("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'persona:facilitator'")
  .get(convergeCase.sessionId) as { raw_content: string } | undefined;
assert.ok(convUtt?.raw_content.includes("【収束】"), "収束まとめが投稿される");
const gapStatus = core.client.raw.prepare("SELECT status FROM design_gaps WHERE id = ?").get(convergeCase.gapId) as { status: string };
assert.equal(gapStatus.status, "closed", "止揚到達で gap が closed");
console.log("ok facilitator converge (止揚到達)");

// 収束した議論の session は ended_at が入り、 ダッシュボードの「進行中」から外れる
// (gap closed と session ライフサイクルの一致)。収束していない議論は開いたまま。
const convSess = core.client.raw
  .prepare("SELECT ended_at FROM sessions WHERE id = ?")
  .get(convergeCase.sessionId) as { ended_at: number | null };
assert.ok(convSess.ended_at != null, "収束した議論の session は ended_at が入る");
const expandSess = core.client.raw
  .prepare("SELECT ended_at FROM sessions WHERE id = ?")
  .get(expandCase.sessionId) as { ended_at: number | null };
assert.equal(expandSess.ended_at, null, "収束していない議論の session は開いたまま");
console.log("ok facilitator converge ends session (ended_at)");

peDb.close();
core.close();
console.log("facilitator tests: all passed");
