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
const opts = { idleGapMs: 1000, maxPersonas: 20 };
// 新発言あり → active + 再 arm
assert.deepEqual(
  evaluate({ utteranceCount: 5, distinctPersonas: 3, lastActivityAt: 0, now: 9999 }, { lastUtteranceCount: 4, armed: false }, opts),
  { mode: "active", state: { lastUtteranceCount: 5, armed: true } }
);
// idle + armed + persona<=max → expand
assert.equal(
  evaluate({ utteranceCount: 5, distinctPersonas: 3, lastActivityAt: 0, now: 5000 }, { lastUtteranceCount: 5, armed: true }, opts).mode,
  "expand"
);
// idle + armed + persona>max → converge
assert.equal(
  evaluate({ utteranceCount: 5, distinctPersonas: 21, lastActivityAt: 0, now: 5000 }, { lastUtteranceCount: 5, armed: true }, opts).mode,
  "converge"
);
// idle だが disarm 済 → wait (連発しない)
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

// ───────── 2. tickOnce (mock LLM で expand / converge) ─────────
const workDir = path.resolve(".tmp/facilitator");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "kg.kuzu"), path.join(workDir, "events.jsonl"));
const peDb = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(peDb);
const personas = new PersonasRepo(peDb);
const adapter = createDiscatierContextProvider(core);
const ws = "knowledge";
const OLD = Date.now() - 5_000; // idle 化

function makeDiscussion(title: string, personaSpeakers: string[]): { gapId: string; sessionId: string } {
  const gapId = core.repos.designGap.create({ workspaceId: ws, title });
  const sessionId = core.repos.session.create({
    workspaceId: ws,
    title: `discussion-of-gap:${gapId}`,
    startedAt: OLD,
    scene: "discord:1/2",
  } as never);
  personaSpeakers.forEach((sp, i) =>
    core.repos.utterance.create({ workspaceId: ws, sessionId, speakerId: sp, rawContent: `発言${i}`, postedAt: OLD })
  );
  return { gapId, sessionId };
}

const expandCase = makeDiscussion("拡張テスト", ["persona:advocate"]); // persona 1 <= max(2)
const convergeCase = makeDiscussion("収束テスト", ["persona:a", "persona:b", "persona:c"]); // 3 > max(2)

const mockLlm: LLMClient = {
  async invoke(args) {
    if (args.system?.includes("収束")) {
      return { ok: true, text: '{ "summary": "合意: 自動攻撃が核。 対立: 難易度。 暫定結論: ビルド設計が肝。" }' };
    }
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
  options: { idleGapMs: 1000, maxPersonas: 2 },
});

await fac.tickOnce();

// expand: 新 persona 発話が入った
const expandUtts = core.client.raw
  .prepare("SELECT speaker_id, raw_content FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:dyn-%'")
  .all(expandCase.sessionId) as Array<{ speaker_id: string; raw_content: string }>;
assert.equal(expandUtts.length, 1, "停滞 discussion に新 persona が 1 体投入される");
assert.ok(expandUtts[0].raw_content.includes("時間に見合う"), "生成された開口一番が投稿される");
const newPersonaId = expandUtts[0].speaker_id.slice("persona:".length);
assert.ok(personas.get(newPersonaId), "生成 persona が personas に永続化される");
console.log("ok facilitator expand");

// converge: まとめ発言 + gap closed
const convUtt = core.client.raw
  .prepare("SELECT raw_content FROM utterances WHERE session_id = ? AND speaker_id = 'persona:facilitator'")
  .get(convergeCase.sessionId) as { raw_content: string } | undefined;
assert.ok(convUtt?.raw_content.includes("【収束】"), "収束まとめが投稿される");
const gapStatus = core.client.raw.prepare("SELECT status FROM design_gaps WHERE id = ?").get(convergeCase.gapId) as { status: string };
assert.equal(gapStatus.status, "closed", "persona 過多で gap が closed");
console.log("ok facilitator converge");

// 収束後は管理対象外 (再 tick で何も起きない)
await fac.tickOnce();
const convUtt2 = core.client.raw
  .prepare("SELECT COUNT(*) n FROM utterances WHERE session_id = ? AND speaker_id = 'persona:facilitator'")
  .get(convergeCase.sessionId) as { n: number };
assert.equal(convUtt2.n, 1, "収束済 discussion には二度と介入しない");
console.log("ok facilitator stops after convergence");

peDb.close();
core.close();
console.log("facilitator tests: all passed");
