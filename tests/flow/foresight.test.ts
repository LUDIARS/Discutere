/**
 * 未来予測フェーズ (spec/feature/flow/foresight.md) のテスト。
 * - キャスト: 賛成派→改革派 / 反対派→保守派 / 意見屋は少ない陣営へ / 片陣営だけなら行わない
 * - 話者順: 改革派から交互
 * - prompt: イノベーション理論・3/5/10 年後・利益率/確度/ノウハウ強度を含む
 * - ペーパー末尾の「# 未来予測」節は付け直しで重ならない
 * - フェーズ: 提案 → 改革派/保守派 → 予測の順に発話し、LLM 失敗は発話にエラーを出す
 * - dispatch 後段: 有効なら結果に提案/予測と発話が足され、無効なら何もしない
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import type { LLMClient, LLMInvokeArgs, LLMResult } from "../../src/persona-engine/llm/client.js";

const TMP_DIR = path.resolve(".tmp/flow-foresight-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "foresight.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();
const { _resetConfig } = await import("../../src/config.js");
_resetConfig();

const { buildForesightCast, buildCampTurnOrder } = await import("../../src/flow/foresight/camps.js");
const { buildProposalPrompt, buildCampTurnPrompt, buildForecastPrompt } = await import(
  "../../src/flow/foresight/prompts.js"
);
const { appendForesightMarkdown, stripForesight } = await import("../../src/flow/foresight/markdown.js");
const { runForesightPhase, splitForPost } = await import("../../src/flow/foresight/phase.js");
const { dispatchFlow } = await import("../../src/flow/dispatch.js");
const { composeDisplayName } = await import("../../src/flow/persona-display.js");
type FlowUtteranceRecord = import("../../src/flow/director.js").FlowUtteranceRecord;

function rec(over: Partial<FlowUtteranceRecord>): FlowUtteranceRecord {
  return {
    id: over.id ?? `u-${Math.random()}`,
    sessionId: "s-fs",
    paperId: "p-fs",
    round: 1,
    turn: 1,
    personaId: "x",
    personaName: "X",
    role: "debater",
    stance: "pro",
    text: "発言",
    isError: false,
    ...over,
  };
}

const mainUtterances: FlowUtteranceRecord[] = [
  rec({ personaId: "f", personaName: "司会", role: "facilitator", stance: "neutral", turn: 0 }),
  rec({ personaId: "a", personaName: "賛成A", stance: "pro" }),
  rec({ personaId: "b", personaName: "反対B", stance: "con" }),
  rec({ personaId: "c", personaName: "意見C", role: "opinion", stance: "opinion", round: 2 }),
  rec({ personaId: "a", personaName: "賛成A", stance: "pro", round: 2 }),
  rec({ personaId: "e", personaName: "壊れE", stance: "con", isError: true }),
];

// ── キャスト ──
{
  const cast = buildForesightCast(mainUtterances)!;
  assert.ok(cast, "両陣営が揃えばキャストを作る");
  assert.equal(cast.facilitator.personaName, "司会", "進行役は本議論の facilitator");
  const camp = Object.fromEntries(cast.speakers.map((s) => [s.personaName, s.camp]));
  assert.equal(camp["賛成A"], "reform", "賛成派 → 改革派");
  assert.equal(camp["反対B"], "conservative", "反対派 → 保守派");
  assert.equal(camp["意見C"], "reform", "意見屋は少ない陣営 (同数なら改革派) へ");
  assert.equal(camp["壊れE"], undefined, "エラー発話だけの話者は入れない");
  assert.equal(cast.speakers.length, 3, "話者は重複なし");

  assert.equal(buildForesightCast([rec({ personaId: "a", stance: "pro" })]), null, "片陣営だけなら行わない");
  assert.equal(buildForesightCast([]), null, "発話なしなら行わない");
  const noFacilitator = buildForesightCast([rec({ personaId: "a", stance: "pro" }), rec({ personaId: "b", stance: "con" })])!;
  assert.equal(noFacilitator.facilitator.personaName, "進行役", "facilitator 不在は代役");

  const order = buildCampTurnOrder(cast.speakers, 5).map((s) => s.camp);
  assert.deepEqual(order, ["reform", "conservative", "reform", "conservative", "reform"], "改革派から交互");
  console.log("  [ok] foresight: キャスト / 話者順");
}

// ── prompt ──
{
  const proposal = buildProposalPrompt("ガチャ課金の未来", "ガチャは続く");
  assert.ok(proposal.system.includes("破壊的イノベーション"), "イノベーション理論");
  assert.ok(proposal.system.includes("【破壊可能な一手】") && proposal.system.includes("【リスク】"), "提案の見出し");
  assert.ok(proposal.prompt.includes("ガチャは続く"), "本議論の結論を渡す");
  assert.ok(buildProposalPrompt("t", "").prompt.includes("(結論なし)"), "結論なしを明示");

  const reform = buildCampTurnPrompt({ theme: "t", proposal: "P", personaName: "A", camp: "reform", lines: [] });
  const conservative = buildCampTurnPrompt({ theme: "t", proposal: "P", personaName: "B", camp: "conservative", lines: [{ personaName: "A", camp: "reform", text: "やるべき" }] });
  assert.ok(reform.system.includes("【改革派 (賛同)】"), "改革派の立場");
  assert.ok(conservative.system.includes("【保守派 (否定)】"), "保守派の立場");
  assert.ok(conservative.prompt.includes("A [改革派]: やるべき"), "相手陣営の発言を渡す");

  const forecast = buildForecastPrompt({ theme: "t", conclusion: "c", proposal: "P", lines: [] });
  for (const head of ["【3年後】", "【5年後】", "【10年後】", "利益率の高さ", "確度", "ノウハウの強度 (真似されにくさ)", "対象外"]) {
    assert.ok(forecast.system.includes(head), `予測 prompt に ${head}`);
  }
  console.log("  [ok] foresight: prompt");
}

// ── ペーパー節 / 分割 / 表示名 ──
{
  const once = appendForesightMarkdown("# 議題\nX\n\n# 結論\nY", { proposal: "P1", forecast: "F1" });
  assert.ok(once.includes("# 未来予測") && once.includes("P1") && once.includes("F1"), "節を付ける");
  const twice = appendForesightMarkdown(once, { proposal: "P2", forecast: "F2" });
  assert.equal(twice.split("# 未来予測").length - 1, 1, "付け直しで重ならない");
  assert.ok(twice.includes("P2") && !twice.includes("P1"), "新しい内容に置き換わる");
  assert.equal(stripForesight(twice), "# 議題\nX\n\n# 結論\nY", "節を外すと元に戻る");
  assert.ok(appendForesightMarkdown("", { proposal: "P", forecast: "" }).startsWith("# 未来予測"), "空本文でも付く");

  const long = ["a".repeat(1000), "b".repeat(1000), "c".repeat(4000)].join("\n");
  const chunks = splitForPost(long);
  assert.ok(chunks.every((c) => c.length <= 1800), "1 投稿の上限以下");
  assert.equal(chunks.join("").replace(/\n/g, ""), long.replace(/\n/g, ""), "内容を失わない");

  assert.equal(composeDisplayName({ name: "A", stance: "reform" }), "A (改革派)");
  assert.equal(composeDisplayName({ name: "B", stance: "conservative" }), "B (保守派)");
  console.log("  [ok] foresight: ペーパー節 / 分割 / 表示名");
}

/** location ごとに決まった応答を返す LLM (cost log の location は prompt からは見えないので system で判定)。 */
function makeLlm(opts: { failProposal?: boolean; failCampOnce?: boolean } = {}) {
  const calls: string[] = [];
  let campFailed = false;
  const llm: LLMClient = {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      const sys = args.system ?? "";
      if (sys.includes("提案する")) {
        calls.push("proposal");
        if (opts.failProposal) return { ok: false, error: "boom" };
        return { ok: true, text: "【破壊可能な一手】無料で配る" };
      }
      if (sys.includes("未来を予測する")) {
        calls.push("forecast");
        return { ok: true, text: "【3年後】a\n【5年後】b\n【10年後】c\n【ビジネス分析】利益率: 高" };
      }
      calls.push("camp");
      if (opts.failCampOnce && !campFailed) {
        campFailed = true;
        return { ok: false, error: "camp-boom" };
      }
      return { ok: true, text: sys.includes("【改革派") ? "壊せる" : "守るべき" };
    },
  };
  return { llm, calls };
}

// ── フェーズ本体 ──
{
  const { llm, calls } = makeLlm({ failCampOnce: true });
  const committed: FlowUtteranceRecord[] = [];
  let body = "# 議題\nX";
  const result = await runForesightPhase({
    theme: "ガチャ課金の未来",
    sessionId: "s-fs",
    paperId: "p-fs",
    flow: "discussion",
    llm,
    utterances: mainUtterances,
    conclusion: "ガチャは続く",
    turns: 4,
    commit: async (r) => {
      committed.push(r);
    },
    paperBody: { get: () => body, set: (md) => (body = md) },
    log: () => {},
    warn: () => {},
  });
  assert.ok(result, "フェーズを行う");
  assert.deepEqual(calls, ["proposal", "camp", "camp", "camp", "camp", "forecast"], "提案 → 議論 4 回 → 予測");
  assert.equal(result!.proposal, "【破壊可能な一手】無料で配る");
  assert.ok(result!.forecast.includes("【10年後】"));
  assert.ok(committed.every((r) => r.round === 3), "本議論の最終ラウンドの次のラウンド");
  assert.deepEqual(committed.map((r) => r.turn), committed.map((_, i) => i), "turn は通し番号");
  assert.ok(committed[0].text.startsWith("🔮") && committed[0].stance === "neutral", "先頭は進行役の提案");
  assert.ok(committed[committed.length - 1].text.startsWith("🔭"), "最後は予測");
  const camps = committed.slice(1, -1);
  assert.equal(camps[0].isError, true, "LLM 失敗は発話にエラーを出す");
  assert.deepEqual(camps.slice(1).map((r) => r.stance), ["conservative", "reform", "conservative"]);
  assert.ok(body.includes("# 未来予測") && body.includes("無料で配る"), "ペーパーに節を焼く");
  console.log("  [ok] foresight: 提案 → 改革派/保守派 → 予測");
}
{
  const { llm, calls } = makeLlm({ failProposal: true });
  const committed: FlowUtteranceRecord[] = [];
  const result = await runForesightPhase({
    theme: "t",
    sessionId: "s-fs",
    paperId: "p-fs",
    flow: "discussion",
    llm,
    utterances: mainUtterances,
    conclusion: "",
    turns: 4,
    commit: async (r) => {
      committed.push(r);
    },
    log: () => {},
    warn: () => {},
  });
  assert.deepEqual(calls, ["proposal"], "提案に失敗したら以降を行わない");
  assert.equal(result!.proposal, "");
  assert.equal(committed.length, 1);
  assert.ok(committed[0].isError, "失敗を発話に出す");
  console.log("  [ok] foresight: 提案失敗で止める");
}

// ── dispatch 後段 ──
{
  const fakeResult = {
    sessionId: "s-dispatch",
    paperId: "p-dispatch",
    utterances: mainUtterances.map((u) => ({ ...u, sessionId: "s-dispatch", paperId: "p-dispatch" })),
    rounds: 2,
    conclusion: "結論X",
    concluded: true,
  };
  const { llm } = makeLlm();
  const notified: FlowUtteranceRecord[] = [];
  const deps = {
    llm,
    onUtterance: (u: FlowUtteranceRecord) => {
      notified.push(u);
    },
    log: () => {},
    warn: () => {},
    drivers: { discussion: async () => ({ ...fakeResult, utterances: [...fakeResult.utterances] }) },
  };

  const on = await dispatchFlow({ theme: "t", tags: [], flow: "議論" }, deps);
  assert.equal(on.kind, "discussion");
  if (on.kind === "discussion") {
    assert.ok(on.result.foresight?.forecast.includes("【3年後】"), "結果に予測が付く");
    assert.equal(on.result.utterances.length, fakeResult.utterances.length + notified.length, "発話が足される");
    assert.ok(notified.length >= 3, "onUtterance で通知する");
    assert.equal(on.result.conclusion, "結論X", "本議論の結論は変えない");
  }

  process.env.DISCUTERE_FLOW_FORESIGHT_ENABLED = "false";
  _resetConfig();
  notified.length = 0;
  const off = await dispatchFlow({ theme: "t", tags: [], flow: "議論" }, deps);
  if (off.kind === "discussion") {
    assert.equal(off.result.foresight, undefined, "無効なら付けない");
    assert.equal(notified.length, 0, "無効なら発話しない");
  }
  delete process.env.DISCUTERE_FLOW_FORESIGHT_ENABLED;
  _resetConfig();
  console.log("  [ok] foresight: dispatch 後段 (有効 / 無効)");
}

console.log("foresight.test.ts: all passed");
