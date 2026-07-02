/**
 * respec 01 — ペルソナ状態 (stance セッション固定 + 価値軸 + flow_session_persona) テスト。
 * - debater の pro/con 均等割 (偶数 2:2 / 奇数は rng 固定で決定的)
 * - personaStance が純関数 (旧 decideStance のターン再抽選なし)
 * - 統合: 同一 persona の stance が全発話で不変 (flow_utterance を検証)
 * - 価値軸: setupSessionPersonas 成功時に valueAxis/coreClaims がマージされ prompt に注入される
 * - 価値軸 LLM 失敗: warn が出て議論は完走、valueAxis 無しの prompt
 * - flow_session_persona にキャスト全員が永続される
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";
import type { LLMInvokeArgs, LLMResult } from "../../src/persona-engine/llm/client.js";

const TMP_DIR = path.resolve(".tmp/flow-persona-state-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "persona-state.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();
const { _resetConfig } = await import("../../src/config.js");
_resetConfig();

const { generateFlowPersonas, personaStance } = await import("../../src/flow/personas.js");
const { setupSessionPersonas, buildPersonaSetupPrompt } = await import("../../src/flow/persona-setup.js");
const { buildPersonaUserPrompt } = await import("../../src/flow/discussion-paper.js");
const { runDiscussionFlow } = await import("../../src/flow/director.js");
const { MockLLMClient } = await import("../../src/persona-engine/llm/mock.js");

function lcg(seedInit: number) {
  let seed = seedInit;
  return () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };
}

// ── debater 均等割: 4 人 → pro 2 / con 2 ──────────────────────────────────────
{
  // count=6 → facilitator 1 + opinion 1 + debater 4
  const personas = generateFlowPersonas({ count: 6, defaultModel: "m", isLocal: false, rng: lcg(1) });
  const debaters = personas.filter((p) => p.role === "debater");
  assert.equal(debaters.length, 4, "debater 4 人");
  assert.equal(debaters.filter((d) => d.stance === "pro").length, 2, "pro 2");
  assert.equal(debaters.filter((d) => d.stance === "con").length, 2, "con 2");
  assert.equal(personas[0].stance, "neutral", "facilitator は neutral");
  assert.ok(personas.filter((p) => p.role === "opinion").every((p) => p.stance === "opinion"), "opinion 役は opinion");
  console.log("  [ok] generateFlowPersonas: debater 4 → pro 2 / con 2");
}

// ── debater 奇数割: 5 人 → 3:2 or 2:3 (rng 固定で決定的) ──────────────────────
{
  // count=8 → facilitator 1 + opinion 2 + debater 5
  const run = (seed: number) => {
    const personas = generateFlowPersonas({ count: 8, defaultModel: "m", isLocal: false, rng: lcg(seed) });
    const debaters = personas.filter((p) => p.role === "debater");
    assert.equal(debaters.length, 5, "debater 5 人");
    const pro = debaters.filter((d) => d.stance === "pro").length;
    const con = debaters.filter((d) => d.stance === "con").length;
    assert.ok(
      (pro === 3 && con === 2) || (pro === 2 && con === 3),
      `5 人は 3:2 or 2:3 (got ${pro}:${con})`
    );
    return `${pro}:${con}`;
  };
  assert.equal(run(77), run(77), "rng 固定で割当が決定的");
  console.log("  [ok] generateFlowPersonas: debater 5 → 3:2 or 2:3, deterministic with fixed rng");
}

// ── personaStance: 純関数 (rng 依存なし) ─────────────────────────────────────
{
  const personas = generateFlowPersonas({ count: 4, defaultModel: "m", isLocal: false, rng: lcg(5) });
  for (const p of personas) {
    const s1 = personaStance(p);
    for (let i = 0; i < 10; i++) {
      assert.equal(personaStance(p), s1, `${p.name} の stance は呼ぶたびに不変`);
    }
    if (p.role === "facilitator") assert.equal(s1, "neutral");
    if (p.role === "opinion") assert.equal(s1, "opinion");
    if (p.role === "debater") assert.ok(s1 === "pro" || s1 === "con");
  }
  console.log("  [ok] personaStance: pure (no per-turn re-roll)");
}

// ── setupSessionPersonas: 価値軸/核主張のマージ + flow_session_persona 永続 ─────
{
  _resetFlowDb();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "setup.db");

  const personas = generateFlowPersonas({ count: 4, defaultModel: "m", isLocal: false, rng: lcg(11) });
  const debater = personas.find((p) => p.role === "debater")!;
  const opinion = personas.find((p) => p.role === "opinion")!;

  // prompt にキャスト一覧 (id/ロール/スタンス) が載る
  const setupPrompt = buildPersonaSetupPrompt("テーマX", personas);
  for (const p of personas) {
    assert.ok(setupPrompt.includes(p.id), `setup prompt に ${p.name} の id`);
  }
  assert.ok(setupPrompt.includes("スタンス: pro") || setupPrompt.includes("スタンス: con"), "スタンスを載せる");

  // LLM は prompt から id を読んで全員ぶんの割当 JSON を返す
  const mock = {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      const assignments = personas.map((p) => ({
        personaId: p.id,
        valueAxis: `${p.name}の価値軸`,
        coreClaims: [`${p.name}の主張`],
      }));
      assert.ok(args.prompt.includes("テーマX"), "テーマが prompt に載る");
      return { ok: true, text: JSON.stringify(assignments) };
    },
  };

  const warns: string[] = [];
  const enriched = await setupSessionPersonas({
    theme: "テーマX",
    sessionId: "sess-setup-1",
    personas,
    llm: mock,
    warn: (m) => warns.push(m),
  });

  assert.equal(warns.length, 0, "成功時に warn なし");
  const eDebater = enriched.find((p) => p.id === debater.id)!;
  const eOpinion = enriched.find((p) => p.id === opinion.id)!;
  assert.equal(eDebater.valueAxis, `${debater.name}の価値軸`, "debater に valueAxis");
  assert.deepEqual(eDebater.coreClaims, [`${debater.name}の主張`], "debater に coreClaims");
  assert.equal(eOpinion.valueAxis, `${opinion.name}の価値軸`, "opinion にも valueAxis");
  assert.equal(eOpinion.coreClaims, undefined, "coreClaims は debater のみ採用");

  // flow_session_persona に全員永続
  const db = new Database(process.env.DATABASE_PATH);
  const rows = db
    .prepare("SELECT id, name, role, stance, value_axis, core_claims_json FROM flow_session_persona WHERE session_id = ?")
    .all("sess-setup-1") as Array<Record<string, unknown>>;
  db.close();
  assert.equal(rows.length, personas.length, "キャスト全員が永続される");
  const dRow = rows.find((r) => r.id === debater.id)!;
  assert.equal(dRow.stance, debater.stance, "stance が永続される");
  assert.equal(dRow.value_axis, `${debater.name}の価値軸`, "value_axis が永続される");
  assert.deepEqual(JSON.parse(dRow.core_claims_json as string), [`${debater.name}の主張`], "core_claims 永続");

  // prompt 注入: 価値軸 + (debater のみ) 核主張
  const paper = {
    theme: "t", supplement: "", mechanicsText: "(メカニクスデータなし)",
    previousRoundsText: "(前ラウンドなし)", currentRoundUtterances: [],
  };
  const dPrompt = buildPersonaUserPrompt(paper, "pro", eDebater);
  assert.ok(dPrompt.includes(`あなたが重視する価値: ${debater.name}の価値軸`), "価値軸が prompt に注入される");
  assert.ok(dPrompt.includes(`あなたの核となる主張: ${debater.name}の主張`), "核主張が prompt に注入される");
  const oPrompt = buildPersonaUserPrompt(paper, "opinion", eOpinion);
  assert.ok(oPrompt.includes("あなたが重視する価値:"), "opinion にも価値軸");
  assert.ok(!oPrompt.includes("あなたの核となる主張"), "opinion に核主張は出さない");
  console.log("  [ok] setupSessionPersonas: merge + persist + prompt injection");
}

// ── 価値軸 LLM 失敗: warn + 議論は完走 (valueAxis なしで degrade) ───────────────
{
  _resetFlowDb();
  _resetConfig();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "degrade.db");

  const warns: string[] = [];
  const mock = new MockLLMClient([
    () => ({ ok: false as const, error: "setup boom" }), // persona-setup が失敗
  ], "通常の発話です。");

  const result = await runDiscussionFlow("degrade テーマ", [], {
    llm: mock,
    rng: lcg(21),
    rounds: 1,
    turnsPerRound: 2,
    possess: false,
    gamesDir: path.join(TMP_DIR, "no-games"),
    log: () => {},
    warn: (m) => warns.push(m),
  });

  assert.ok(warns.some((w) => w.includes("ペルソナ価値軸生成 失敗")), "価値軸失敗の warn が出る");
  assert.ok(result.utterances.length > 0, "議論は完走する");
  assert.ok(result.utterances.every((u) => !u.text.includes("あなたが重視する価値")), "発話は正常");

  // 失敗でもキャストは flow_session_persona に永続される (value_axis は NULL)
  const db = new Database(process.env.DATABASE_PATH);
  const rows = db
    .prepare("SELECT value_axis FROM flow_session_persona WHERE session_id = ?")
    .all(result.sessionId) as Array<{ value_axis: string | null }>;
  db.close();
  assert.ok(rows.length > 0, "キャストは失敗時も永続される");
  assert.ok(rows.every((r) => r.value_axis === null), "value_axis は NULL (degrade)");
  console.log("  [ok] persona-setup degrade: warn + flow completes + cast persisted without value axis");
}

// ── 統合: 同一 persona の stance が全発話で不変 ────────────────────────────────
{
  _resetFlowDb();
  _resetConfig();
  process.env.DATABASE_PATH = path.join(TMP_DIR, "invariant.db");

  const mock = new MockLLMClient([], "スタンス不変テストの発話。");
  const result = await runDiscussionFlow("stance 不変テーマ", [], {
    llm: mock,
    rng: lcg(31),
    rounds: 2,
    turnsPerRound: 3,
    possess: false,
    gamesDir: path.join(TMP_DIR, "no-games"),
    log: () => {},
    warn: () => {},
  });

  const db = new Database(process.env.DATABASE_PATH);
  const rows = db
    .prepare("SELECT persona_id, role, stance FROM flow_utterance WHERE session_id = ?")
    .all(result.sessionId) as Array<{ persona_id: string; role: string; stance: string }>;
  db.close();

  const byPersona = new Map<string, Set<string>>();
  for (const r of rows) {
    if (!byPersona.has(r.persona_id)) byPersona.set(r.persona_id, new Set());
    byPersona.get(r.persona_id)!.add(r.stance);
  }
  for (const [pid, stances] of byPersona) {
    assert.equal(stances.size, 1, `persona ${pid} の stance が全発話で不変 (got ${[...stances].join(",")})`);
  }
  const debaterStances = rows.filter((r) => r.role === "debater").map((r) => r.stance);
  assert.ok(debaterStances.every((s) => s === "pro" || s === "con"), "debater は pro/con のみ");
  console.log(`  [ok] integration: stance invariant across ${rows.length} utterances`);
}

delete process.env.DATABASE_PATH;
_resetFlowDb();
console.log("persona-state (respec 01) tests: all passed");
