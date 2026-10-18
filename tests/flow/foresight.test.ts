/**
 * 未来予測フェーズ (spec/feature/flow/foresight.md) のテスト。
 * - キャスト: 陣営は本議論の賛否と独立に初登場順で交互 / 本議論の発言を持つ / 話者 2 人未満なら行わない
 * - 話者順: 改革派から交互、往復単位 (turns=1 でも保守派が話す)、段階は 成立条件 → 反証 → 確かめ方
 * - prompt: 理論の適用条件・「当てはまらない」・前提事実・条件付きシナリオ・算定不能・文脈の受け渡し
 * - ペーパー末尾の節は付け直しで重ならない (先頭見出し・末尾改行なしも外す)
 * - 分割: コードポイントで数え、複数に分けたら (n/m) を付ける
 * - フェーズ: 提案 → 改革派/保守派 → 予測の順、失敗は発話にエラーを出し status に反映
 * - dispatch 後段: タグ / 機能無効 / 未収束で省略 / 状態の記録 / 本議論の例外
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

const { buildForesightCast, buildCampTurnOrder, normalizeCampTurns, campStageAt } = await import(
  "../../src/flow/foresight/camps.js"
);
const { buildProposalPrompt, buildCampTurnPrompt, buildForecastPrompt, contextBlock } = await import(
  "../../src/flow/foresight/prompts.js"
);
const { appendForesightMarkdown, stripForesight } = await import("../../src/flow/foresight/markdown.js");
const { runForesightPhase, splitForPost, labelParts } = await import("../../src/flow/foresight/phase.js");
const { getForesightStatus, isForesightActive, setForesightStatus, _resetForesightStatus } = await import(
  "../../src/flow/foresight/state.js"
);
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
  rec({ personaId: "a", personaName: "賛成A", stance: "pro", text: "A の 1 回目" }),
  rec({ personaId: "b", personaName: "反対B", stance: "con", text: "B の 1 回目" }),
  rec({ personaId: "c", personaName: "意見C", role: "opinion", stance: "opinion", round: 2 }),
  rec({ personaId: "a", personaName: "賛成A", stance: "pro", round: 2, text: "A の 2 回目" }),
  rec({ personaId: "a", personaName: "賛成A", stance: "pro", round: 2, text: "A の 3 回目" }),
  rec({ personaId: "e", personaName: "壊れE", stance: "con", isError: true }),
];

// ── キャスト ──
{
  const cast = buildForesightCast(mainUtterances)!;
  assert.ok(cast, "話者が 2 人以上ならキャストを作る");
  assert.equal(cast.facilitator.personaName, "司会", "進行役は本議論の facilitator");
  const camp = Object.fromEntries(cast.speakers.map((s) => [s.personaName, s.camp]));
  assert.equal(camp["賛成A"], "reform", "初登場順の 1 人目 → 改革派");
  assert.equal(camp["反対B"], "conservative", "2 人目 → 保守派");
  assert.equal(camp["意見C"], "reform", "3 人目 → 改革派 (本議論の賛否とは独立)");
  assert.equal(camp["壊れE"], undefined, "エラー発話だけの話者は入れない");
  assert.equal(cast.speakers.length, 3, "話者は重複なし");
  const a = cast.speakers.find((s) => s.personaName === "賛成A")!;
  assert.deepEqual(a.ownStatements, ["A の 2 回目", "A の 3 回目"], "本議論の直近の発言を 2 件持つ");

  // 賛否と陣営は独立: 反対派が先に話せば反対派が改革派になる
  const conFirst = buildForesightCast([rec({ personaId: "b", stance: "con" }), rec({ personaId: "a", stance: "pro" })])!;
  assert.equal(conFirst.speakers[0].camp, "reform", "反対派でも初登場が先なら改革派");

  assert.equal(buildForesightCast([rec({ personaId: "a", stance: "pro" })]), null, "話者 1 人なら行わない");
  assert.equal(buildForesightCast([]), null, "発話なしなら行わない");
  const noFacilitator = buildForesightCast([rec({ personaId: "a", stance: "pro" }), rec({ personaId: "b", stance: "con" })])!;
  assert.equal(noFacilitator.facilitator.personaName, "進行役", "facilitator 不在は代役");

  const long = buildForesightCast([rec({ personaId: "a", text: "あ".repeat(500) }), rec({ personaId: "b" })])!;
  assert.ok(Array.from(long.speakers[0].ownStatements[0]).length <= 241, "本議論の発言は字数を切る");

  const order = buildCampTurnOrder(cast.speakers, 6).map((s) => s.camp);
  assert.deepEqual(order, ["reform", "conservative", "reform", "conservative", "reform", "conservative"], "改革派から交互");
  assert.equal(normalizeCampTurns(1), 2, "turns=1 でも 1 往復 (保守派が話す)");
  assert.equal(normalizeCampTurns(5), 6, "奇数は往復単位に切り上げ");
  assert.equal(buildCampTurnOrder(cast.speakers, 1).length, 2);
  assert.deepEqual(
    [0, 1, 2, 3, 4, 5, 6, 7].map(campStageAt),
    ["conditions", "conditions", "refutation", "refutation", "verification", "verification", "verification", "verification"],
    "往復ごとに 成立条件 → 反証 → 確かめ方"
  );
  console.log("  [ok] foresight: キャスト / 話者順 / 段階");
}

// ── prompt ──
{
  const context = {
    paperMd: "# 議題\nガチャ\n\n# 議論の経過\n止揚: 段階導入",
    voices: [{ content: "高すぎる", source: "steam" }],
  };
  const proposal = buildProposalPrompt("ガチャ課金の未来", "ガチャは続く", context);
  assert.ok(proposal.system.includes("ローエンド型") && proposal.system.includes("非消費者"), "破壊の型の適用条件");
  assert.ok(proposal.system.includes("キャズム (Moore)") && proposal.system.includes("Rogers とは別の理論"), "Rogers と Moore を分ける");
  assert.ok(proposal.system.includes("当てはまらない"), "当てはまらないを許す");
  assert.ok(proposal.system.includes("無料 / 有料の区分ではない"), "普及区分の誤用を防ぐ");
  for (const head of ["【前提事実】", "【理論の当てはめ】", "【破壊可能な一手】", "【実行すると何が起きるか】", "【リスク】"]) {
    assert.ok(proposal.system.includes(head), `提案の見出し ${head}`);
  }
  assert.ok(!proposal.system.includes("何を過剰に供給しているか"), "過剰供給を前提にした問いを使わない");
  assert.ok(proposal.system.includes("未確認"), "根拠が無いものは未確認と書かせる");
  assert.ok(proposal.prompt.includes("ガチャは続く"), "本議論の結論を渡す");
  assert.ok(proposal.prompt.includes("止揚: 段階導入"), "ペーパー本文 (止揚を含む) を渡す");
  assert.ok(proposal.prompt.includes("「高すぎる」(出所: steam)") && proposal.prompt.includes("関連は未確認"), "外部の声を出所と注意書き付きで渡す");
  assert.ok(buildProposalPrompt("t", "").prompt.includes("(結論なし)"), "結論なしを明示");
  assert.equal(contextBlock(undefined), "", "文脈なしなら何も足さない");
  assert.ok(contextBlock({ paperMd: "x".repeat(7000), voices: [] }).includes("(以下略)"), "長いペーパーは切る");

  const reform = buildCampTurnPrompt({ theme: "t", proposal: "P", personaName: "A", camp: "reform", lines: [], ownStatements: ["前に言ったこと"] });
  const conservative = buildCampTurnPrompt({
    theme: "t",
    proposal: "P",
    personaName: "B",
    camp: "conservative",
    lines: [{ personaName: "A", camp: "reform", text: "やるべき" }],
    stage: "verification",
  });
  assert.ok(reform.system.includes("【改革派 (賛同)】") && reform.system.includes("本議論での自分の賛否とは独立"), "改革派の立場は本議論と独立");
  assert.ok(conservative.system.includes("【保守派 (否定)】"), "保守派の立場");
  assert.ok(reform.system.includes("成り立つための条件"), "既定の段階は成立条件");
  assert.ok(conservative.system.includes("確かめ方"), "段階の論点を入れる");
  assert.ok(reform.prompt.includes("前に言ったこと"), "本議論での自分の発言を渡す");
  assert.ok(conservative.prompt.includes("A [改革派]: やるべき"), "相手陣営の発言を渡す");

  const forecast = buildForecastPrompt({ theme: "t", conclusion: "c", proposal: "P", lines: [], context });
  for (const p of [proposal, reform, conservative, forecast]) {
    assert.ok(p.system.includes("結論を覆したり書き換えたりしない"), "どの prompt もアドバイザリー扱い");
  }
  for (const head of [
    "【未解決の前提】",
    "勝ち負けは判定しない",
    "【3年後】【5年後】【10年後】",
    "成立",
    "限定的定着",
    "撤退・吸収",
    "先行指標",
    "算定不能",
    "判定不能",
    "利益率の高さ",
    "確度",
    "ノウハウの強度 (真似されにくさ)",
    "対象外",
  ]) {
    assert.ok(forecast.system.includes(head), `予測 prompt に ${head}`);
  }
  assert.ok(!forecast.system.includes("新しい主流と、次に破壊される側"), "成功の筋書きを誘導しない");
  assert.ok(forecast.prompt.includes("止揚: 段階導入"), "予測にもペーパー本文を渡す");
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
  const fromEmpty = appendForesightMarkdown("", { proposal: "P", forecast: "" });
  assert.ok(fromEmpty.startsWith("# 未来予測"), "空本文でも付く");
  assert.equal(stripForesight(fromEmpty), "", "本文の先頭にある見出しも外せる");
  const again = appendForesightMarkdown(fromEmpty, { proposal: "Q", forecast: "" });
  assert.equal(again.split("# 未来予測").length - 1, 1, "先頭見出しの本文に付け直しても重ならない");
  assert.equal(stripForesight("# 結論\nY\n\n# 未来予測アドバイザリー (参考)"), "# 結論\nY", "末尾改行が無い見出しも外す");
  assert.ok(once.includes("# 未来予測アドバイザリー (参考)") && once.includes("結論とは別の参考"), "参考扱いを明記");
  const legacy = "# 結論\nY\n\n# 未来予測\n\n## 破壊的な一手の提案\n旧";
  assert.equal(stripForesight(legacy), "# 結論\nY", "初版の見出しの節も外す");
  assert.equal(stripForesight("# 未来予測の補足ではない見出し\n本文"), "# 未来予測の補足ではない見出し\n本文", "似た別見出しは外さない");

  const long = ["a".repeat(1000), "b".repeat(1000), "c".repeat(4000)].join("\n");
  const chunks = splitForPost(long);
  assert.ok(chunks.every((c) => c.length <= 1800), "1 投稿の上限以下");
  assert.equal(chunks.join("").replace(/\n/g, ""), long.replace(/\n/g, ""), "内容を失わない");
  const emoji = "😀".repeat(2000);
  const emojiChunks = splitForPost(emoji, 100);
  assert.ok(emojiChunks.every((c) => !/[\uD800-\uDBFF]$/.test(c)), "サロゲートペアを割らない");
  assert.equal(emojiChunks.join(""), emoji);
  assert.deepEqual(labelParts(["x"]), ["x"], "1 つなら番号を付けない");
  assert.deepEqual(labelParts(["x", "y"]), ["x\n(1/2)", "y\n(2/2)"], "複数なら (n/m) を付ける");

  assert.equal(composeDisplayName({ name: "A", stance: "reform" }), "A (改革派)");
  assert.equal(composeDisplayName({ name: "B", stance: "conservative" }), "B (保守派)");
  console.log("  [ok] foresight: ペーパー節 / 分割 / 表示名");
}

/** 段ごとに決まった応答を返す LLM (system の文言で段を判定)。 */
function makeLlm(
  opts: { failProposal?: boolean; failCampOnce?: boolean; emptyCampOnce?: boolean; failForecast?: boolean } = {}
) {
  const calls: string[] = [];
  const models: Array<string | undefined> = [];
  const prompts: string[] = [];
  let campFailed = false;
  let campEmptied = false;
  const llm: LLMClient = {
    async invoke(args: LLMInvokeArgs): Promise<LLMResult> {
      const sys = args.system ?? "";
      models.push(args.model);
      prompts.push(args.prompt);
      if (sys.includes("破壊可能な一手を検討する")) {
        calls.push("proposal");
        if (opts.failProposal) return { ok: false, error: "boom" };
        return { ok: true, text: "【破壊可能な一手】無料で配る" };
      }
      if (sys.includes("条件付きのシナリオで示す")) {
        calls.push("forecast");
        if (opts.failForecast) return { ok: false, error: "fc-boom" };
        return { ok: true, text: "【未解決の前提】a\n【3年後】b\n【5年後】c\n【10年後】d\n【ビジネス分析】算定不能" };
      }
      calls.push("camp");
      if (opts.failCampOnce && !campFailed) {
        campFailed = true;
        return { ok: false, error: "camp-boom" };
      }
      if (opts.emptyCampOnce && !campEmptied) {
        campEmptied = true;
        return { ok: true, text: "  " };
      }
      return { ok: true, text: sys.includes("【改革派") ? "壊せる" : "守るべき" };
    },
  };
  return { llm, calls, models, prompts };
}

type ForesightPhaseArgs = import("../../src/flow/foresight/phase.js").ForesightPhaseArgs;

function phaseArgs(
  llm: LLMClient,
  committed: FlowUtteranceRecord[],
  over: Partial<ForesightPhaseArgs> = {}
): ForesightPhaseArgs {
  return {
    theme: "ガチャ課金の未来",
    sessionId: "s-fs",
    paperId: "p-fs",
    flow: "discussion",
    llm,
    utterances: mainUtterances,
    conclusion: "ガチャは続く",
    turns: 4,
    commit: async (r: FlowUtteranceRecord) => {
      committed.push(r);
    },
    log: () => {},
    warn: () => {},
    ...over,
  };
}

// ── フェーズ本体 ──
{
  const { llm, calls, models, prompts } = makeLlm({ failCampOnce: true });
  const committed: FlowUtteranceRecord[] = [];
  let body = "# 議題\nX";
  const result = await runForesightPhase(
    phaseArgs(llm, committed, {
      model: "m-camp",
      forecastModel: "m-forecast",
      context: { paperMd: "# 議題\n資料あり", voices: [] },
      paperBody: { get: () => body, set: (md: string) => (body = md) },
    })
  );
  assert.ok(result, "フェーズを行う");
  assert.deepEqual(calls, ["proposal", "camp", "camp", "camp", "camp", "forecast"], "提案 → 議論 4 回 → 予測");
  assert.deepEqual(models, ["m-camp", "m-camp", "m-camp", "m-camp", "m-camp", "m-forecast"], "予測だけ別モデル");
  assert.ok(prompts[0].includes("資料あり") && prompts[5].includes("資料あり"), "提案と予測に文脈を渡す");
  assert.equal(result!.proposal, "【破壊可能な一手】無料で配る");
  assert.ok(result!.forecast.includes("【10年後】"));
  assert.equal(result!.status, "degraded", "発言の失敗があれば一部失敗");
  assert.equal(result!.failures, 1);
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
  const { llm } = makeLlm();
  const committed: FlowUtteranceRecord[] = [];
  const result = await runForesightPhase(phaseArgs(llm, committed));
  assert.equal(result!.status, "done", "全部出れば完了");
  assert.equal(result!.failures, 0);
}
{
  const { llm, calls } = makeLlm({ failProposal: true });
  const committed: FlowUtteranceRecord[] = [];
  const result = await runForesightPhase(phaseArgs(llm, committed, { conclusion: "" }));
  assert.deepEqual(calls, ["proposal"], "提案に失敗したら以降を行わない");
  assert.equal(result!.proposal, "");
  assert.equal(result!.status, "failed");
  assert.equal(committed.length, 1);
  assert.ok(committed[0].isError, "失敗を発話に出す");
  console.log("  [ok] foresight: 提案失敗で止める");
}
{
  const { llm } = makeLlm({ emptyCampOnce: true, failForecast: true });
  const committed: FlowUtteranceRecord[] = [];
  let body = "# 結論\nY";
  const result = await runForesightPhase(
    phaseArgs(llm, committed, { paperBody: { get: () => body, set: (md: string) => (body = md) } })
  );
  assert.equal(result!.status, "degraded", "空応答と予測失敗は一部失敗");
  assert.equal(result!.failures, 2);
  assert.equal(result!.forecast, "");
  assert.ok(committed[committed.length - 1].isError, "予測失敗を発話に出す");
  assert.ok(body.includes("(予測なし)"), "予測が無くてもペーパーに提案は残す");
}
{
  const { llm } = makeLlm();
  const committed: FlowUtteranceRecord[] = [];
  const result = await runForesightPhase(
    phaseArgs(llm, committed, {
      paperBody: {
        get: () => "# 結論\nY",
        set: () => {
          throw new Error("db locked");
        },
      },
    })
  );
  assert.equal(result!.status, "degraded", "ペーパー追記の失敗も一部失敗に数える");
  console.log("  [ok] foresight: 空応答 / 予測失敗 / ペーパー追記失敗");
}

// ── 進行状態 ──
{
  _resetForesightStatus();
  assert.equal(getForesightStatus("s"), null, "未記録は null");
  setForesightStatus("s", "running");
  assert.equal(isForesightActive("s"), true);
  setForesightStatus("s", "skipped", "理由");
  assert.deepEqual(getForesightStatus("s"), { state: "skipped", detail: "理由" });
  assert.equal(isForesightActive("s"), false);
  _resetForesightStatus();
  console.log("  [ok] foresight: 進行状態");
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
  const { llm, prompts } = makeLlm();
  const notified: FlowUtteranceRecord[] = [];
  let pendingDuringRun: string | undefined;
  const deps = {
    llm,
    sessionId: "s-dispatch",
    onUtterance: (u: FlowUtteranceRecord) => {
      notified.push(u);
    },
    listExternalVoices: () => [{ content: "声その1", source: "niconico" }],
    log: () => {},
    warn: () => {},
    drivers: {
      discussion: async () => {
        pendingDuringRun = getForesightStatus("s-dispatch")?.state;
        return { ...fakeResult, utterances: [...fakeResult.utterances] };
      },
    },
  };

  _resetForesightStatus();
  const untagged = await dispatchFlow({ theme: "t", tags: ["開発"], flow: "議論" }, deps);
  if (untagged.kind === "discussion") {
    assert.equal(untagged.result.foresight, undefined, "未来予測タグが無ければ付けない (オプショナル)");
    assert.equal(notified.length, 0, "タグが無ければ発話しない");
    assert.equal(getForesightStatus("s-dispatch"), null, "タグが無ければ状態も付けない");
  }

  const on = await dispatchFlow({ theme: "t", tags: ["未来予測"], flow: "議論" }, deps);
  assert.equal(on.kind, "discussion");
  assert.equal(pendingDuringRun, "pending", "本議論の実行中から待機中を記録する");
  if (on.kind === "discussion") {
    assert.ok(on.result.foresight?.forecast.includes("【3年後】"), "結果に予測が付く");
    assert.equal(on.result.utterances.length, fakeResult.utterances.length + notified.length, "発話が足される");
    assert.ok(notified.length >= 3, "onUtterance で通知する");
    assert.equal(on.result.conclusion, "結論X", "本議論の結論は変えない");
    assert.equal(getForesightStatus("s-dispatch")?.state, "done", "終わったら完了");
    assert.ok(prompts.some((p) => p.includes("声その1")), "外部の声を未来予測に渡す");
  }

  // 未収束なら行わない
  notified.length = 0;
  const unconcluded = await dispatchFlow(
    { theme: "t", tags: ["未来予測"], flow: "議論" },
    { ...deps, drivers: { discussion: async () => ({ ...fakeResult, conclusion: "", concluded: false }) } }
  );
  if (unconcluded.kind === "discussion") {
    assert.equal(unconcluded.result.foresight, undefined, "未収束なら未来予測を付けない");
    assert.equal(notified.length, 0);
    assert.deepEqual(getForesightStatus("s-dispatch"), { state: "skipped", detail: "本議論が収束していない" });
  }

  // 本議論の例外は状態を失敗にして投げ直す
  await assert.rejects(
    dispatchFlow(
      { theme: "t", tags: ["未来予測"], flow: "議論" },
      {
        ...deps,
        drivers: {
          discussion: async () => {
            throw new Error("engine down");
          },
        },
      }
    ),
    /engine down/
  );
  assert.equal(getForesightStatus("s-dispatch")?.state, "failed", "本議論が落ちたら待機中のまま残さない");

  process.env.DISCUTERE_FLOW_FORESIGHT_ENABLED = "false";
  _resetConfig();
  _resetForesightStatus();
  notified.length = 0;
  const off = await dispatchFlow({ theme: "t", tags: ["未来予測"], flow: "議論" }, deps);
  if (off.kind === "discussion") {
    assert.equal(off.result.foresight, undefined, "機能が無効ならタグがあっても付けない");
    assert.equal(notified.length, 0, "無効なら発話しない");
    assert.equal(getForesightStatus("s-dispatch"), null, "無効なら状態も付けない");
  }
  delete process.env.DISCUTERE_FLOW_FORESIGHT_ENABLED;
  _resetConfig();
  _resetForesightStatus();
  console.log("  [ok] foresight: dispatch 後段 (有効 / 未収束 / 例外 / 無効)");
}

console.log("foresight.test.ts: all passed");
