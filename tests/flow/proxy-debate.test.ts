/**
 * 代理議論 (spec/feature/flow/proxy-debate.md) のテスト。
 *   - 契約 v1 の検証 (仮名以外・URL・個人を特定しうるフィールド・根拠の無い立場カードを拒否)
 *   - 取込の冪等性・内容変更で評価を戻す・失効
 *   - 束 (数値を出さない・予算で根拠を落とす・低信頼は作らない)
 *   - 帰属検査 (束に無い id / 原文に無い引用を本人の発言にしない)
 *   - 立場の割当 (対立したときだけ陣営を作る)
 *   - 議題からの指定の読み取りとキャスト (評価前・失効・期限切れは出さない)
 *   - 形成 (決定的・伏せたゲーム・正負不足で gap=null・リンクを伏せる)
 *   - 評価の合否 (多数派との差・件数不足は未評価)
 *   - dialectic の代理モード E2E (MockLLM): 論点ごとの立場、対立しない論点は結論なし、
 *     束に無い根拠の反論は問いに格下げ、帰属区分と根拠 id が flow_utterance に残る
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";
import Database from "better-sqlite3";

const TMP_DIR = path.resolve(".tmp/flow-proxy-debate-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
const DB_PATH = path.join(TMP_DIR, "test.db");
process.env.DATABASE_PATH = DB_PATH;
process.env.DISCUTERE_FLOW_PROXY_ENABLED = "true";
process.env.DISCUTERE_FLOW_PROXY_REQUIRE_EVAL_PASSED = "false";
process.env.DISCUTERE_FLOW_PROXY_BUDGET_TOKENS = "3500";
process.env.DISCUTERE_FLOW_ROSTER_FACILITATOR = "gpt-facilitator@high";
process.env.DISCUTERE_FLOW_ROSTER_DISCUSSANTS = "claude-a@high,claude-b@high";
process.env.DISCUTERE_FLOW_DIALECTIC_JUDGE_MODEL = "claude-judge@low";

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
const { _resetConfig } = await import("../../src/config.js");
_resetFlowDb();
_resetConfig();

const { parsePersonaPack } = await import("../../src/flow/proxy/contract.js");
const { importPersonaPacks, parsePackInput, proxyLabel } = await import("../../src/flow/proxy/import.js");
const { getProxyProfile, setProxyEvalStatus, isCastable } = await import("../../src/flow/proxy/store.js");
const { renderProxyBundle, estimateTokens } = await import("../../src/flow/proxy/bundle.js");
const { checkAttribution, attributionSuffix, quoteAppearsInEvidence } = await import("../../src/flow/proxy/attribution.js");
const { assignIssueSides, parseProxyStance } = await import("../../src/flow/proxy/stance.js");
const { parseProxyRequest, loadProxyParticipants, ProxyRequestError } = await import("../../src/flow/proxy/cast.js");
const { buildPersonaPack, pickHeldOutGames, sanitizeReviewText } = await import("../../src/flow/proxy/form/build-pack.js");
const { formPersonaPacks, speakerKeyOf } = await import("../../src/flow/proxy/form/group.js");
const { parseGenreDictionary } = await import("../../src/flow/proxy/form/genres.js");
const { judgeProxyEval, scoreRecommendations } = await import("../../src/flow/proxy/eval/metrics.js");
const { resolveProxyRun } = await import("../../src/flow/proxy/resolve.js");
const { runDialecticFlow } = await import("../../src/flow/dialectic/driver.js");

const NOW = 1_800_000_000_000;
const DAY = 24 * 60 * 60 * 1000;

function vec(v = 0.5): number[] {
  return new Array(20).fill(v);
}

function makePack(keySuffix: string, overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    contractVersion: 1,
    personaKind: "observedSpeaker",
    speakerKey: `ext:steam:${keySuffix.padStart(16, "0")}`,
    genre: "roguelike",
    genreSpecVersion: 1,
    profileVersion: `proxy-form-v1:${keySuffix}`,
    formation: { method: "rule", ruleVersion: "proxy-form-v1" },
    window: { observedMonths: null, generatedAt: NOW - DAY, expiresAt: NOW + 30 * DAY },
    sufficiency: { utterances: 12, positive: 7, negative: 5, games: 4, aspects: 4, confidence: "medium", reasons: [] },
    affect: { vector: vec(0.6), vectorSpecVersion: 1 },
    designGap: { vector: vec(0.1), method: "pos-minus-neg", wants: ["asp.difficulty"], complaints: ["asp.content"], reason: null },
    conditionalGaps: [],
    stanceCards: [
      { id: "S1", claim: "毎日の周回を強いる作りは嫌だ", polarity: "neg", evidenceIds: ["E1"], extractor: "rule" },
      { id: "S2", claim: "高難度に何時間でも挑みたい", polarity: "pos", evidenceIds: ["E2"], extractor: "rule" },
    ],
    evidence: [
      { id: "E1", revision: "r1", text: "毎日の周回が義務になるのは本当に嫌だ。続ける気がなくなった。", sourceKind: "steam", gameTitle: "Game A", polarity: "neg", postedAt: null },
      { id: "E2", revision: "r2", text: "週末なら高難度に何時間でも挑みたい。難易度が最高。", sourceKind: "steam", gameTitle: "Game B", polarity: "pos", postedAt: null },
      { id: "E3", revision: "r3", text: "遊べる量が少なくて値段に見合わない。", sourceKind: "steam", gameTitle: "Game C", polarity: "neg", postedAt: null },
    ],
    styleNotes: ["短く書く"],
    likedGames: ["Game B"],
    heldOut: [
      { game: "Game D", recommended: true },
      { game: "Game E", recommended: false },
      { game: "Game F", recommended: true },
    ],
    revoked: false,
    ...overrides,
  };
}

// ── 契約 ────────────────────────────────────────────────────────────────────
{
  assert.ok(parsePersonaPack(makePack("a1")).ok, "正しいパックは通る");
  const raw = parsePersonaPack(makePack("a1", { speakerKey: "ext:steam:76561198000000000" }));
  assert.ok(!raw.ok, "生の SteamID64 は通らない");
  const withUrl = makePack("a1");
  (withUrl.evidence as any[])[0].sourceUrl = "https://steamcommunity.com/x";
  assert.ok(!parsePersonaPack(withUrl).ok, "出典 URL フィールドは通らない");
  const urlInText = makePack("a1");
  (urlInText.evidence as any[])[0].text = "詳しくは https://example.com を見て";
  assert.ok(!parsePersonaPack(urlInText).ok, "本文中の URL も通らない");
  const badCard = makePack("a1", {
    stanceCards: [{ id: "S1", claim: "x の主張です", polarity: "pos", evidenceIds: ["E9"], extractor: "rule" }],
  });
  assert.ok(!parsePersonaPack(badCard).ok, "パックに無い根拠を指す立場カードは通らない");
  const noEvidenceCard = makePack("a1", {
    stanceCards: [{ id: "S1", claim: "根拠の無い主張です", polarity: "pos", evidenceIds: [], extractor: "rule" }],
  });
  assert.ok(!parsePersonaPack(noEvidenceCard).ok, "根拠の無い立場カードは通らない");
  const authorName = makePack("a1", { authorName: "someone" });
  assert.ok(!parsePersonaPack(authorName).ok, "表示名フィールドは通らない");
  console.log("ok proxy contract");
}

// ── 取込 ────────────────────────────────────────────────────────────────────
{
  const first = importPersonaPacks([makePack("a1"), makePack("b2"), { broken: true }], NOW);
  assert.equal(first.inserted, 2);
  assert.equal(first.rejected.length, 1, "壊れた行は理由付きで拒否、残りは取り込む");
  const again = importPersonaPacks([makePack("a1")], NOW);
  assert.equal(again.unchanged, 1, "同じ内容の再取込は何もしない");
  const keyA = makePack("a1").speakerKey as string;
  setProxyEvalStatus(keyA, "roguelike", "passed", "{}", NOW);
  importPersonaPacks([makePack("a1")], NOW);
  assert.equal(getProxyProfile(keyA, "roguelike")!.evalStatus, "passed", "同じ内容なら評価を消さない");
  importPersonaPacks([makePack("a1", { styleNotes: ["長めに書く"] })], NOW);
  assert.equal(getProxyProfile(keyA, "roguelike")!.evalStatus, "unevaluated", "内容が変われば評価を戻す");
  assert.equal(getProxyProfile(keyA, "roguelike")!.label, proxyLabel(keyA));
  assert.ok(/^論者#[0-9a-f]{6}$/.test(proxyLabel(keyA)));

  const jsonl = parsePackInput(`${JSON.stringify(makePack("c3"))}\n{not json}\n`);
  assert.equal(jsonl.length, 2, "JSONL を 1 行ずつ読む");
  const revoked = importPersonaPacks([{ contractVersion: 1, speakerKey: makePack("b2").speakerKey, genre: "roguelike", revoked: true }], NOW);
  assert.equal(revoked.revoked, 1, "最小形の失効通知で失効する");
  const rowB = getProxyProfile(makePack("b2").speakerKey as string, "roguelike")!;
  assert.equal(isCastable(rowB, NOW, false), "失効している");
  importPersonaPacks([makePack("b2")], NOW);
  assert.equal(getProxyProfile(makePack("b2").speakerKey as string, "roguelike")!.revoked, false, "再提供で失効が解ける");
  console.log("ok proxy import");
}

// ── 束 ──────────────────────────────────────────────────────────────────────
{
  const pack = parsePersonaPack(makePack("a1"));
  assert.ok(pack.ok);
  if (pack.ok) {
    const bundle = renderProxyBundle(pack.pack, { budgetTokens: 3500, label: "論者#aaaaaa", focusTerms: ["周回"] })!;
    assert.ok(bundle, "束ができる");
    assert.ok(!/0\.\d/.test(bundle.text), "数値ベクトルを載せない");
    assert.ok(bundle.text.includes("[E1]") && bundle.text.includes("難易度"), "根拠 id と望む観点の日本語が載る");
    assert.ok(bundle.text.includes("本人ではなく"), "AI の代理であることを書く");
    assert.deepEqual(bundle.evidenceIds, ["E1", "E2", "E3"]);
    const small = renderProxyBundle(pack.pack, { budgetTokens: estimateTokens(bundle.text) - 40, label: "論者#aaaaaa", focusTerms: ["周回"] })!;
    assert.ok(small.evidenceIds.length < 3 && small.evidenceIds.includes("E1"), "予算超過は議題と遠い根拠から落とす");
    assert.ok(small.droppedEvidence > 0);
    assert.ok(!small.text.includes("[S2]") || small.evidenceIds.includes("E2"), "落とした根拠だけのカードは載せない");
    assert.equal(renderProxyBundle(pack.pack, { budgetTokens: 50, label: "x" }), null, "根拠 1 件も入らない予算なら作らない");
    const low = { ...pack.pack, sufficiency: { ...pack.pack.sufficiency, confidence: "low" as const } };
    assert.equal(renderProxyBundle(low, { budgetTokens: 3500, label: "x" }), null, "low は束にしない");
  }
  console.log("ok proxy bundle");
}

// ── 帰属 ────────────────────────────────────────────────────────────────────
{
  const scope = { evidenceIds: ["E1", "E2"], evidenceTexts: { E1: "毎日の周回が義務になるのは本当に嫌だ。", E2: "週末なら高難度に挑みたい。" } };
  assert.ok(checkAttribution({ claimKind: "groundedParaphrase", evidenceIds: ["E1"], data: "日課の義務は嫌", scope }).ok);
  const outside = checkAttribution({ claimKind: "groundedParaphrase", evidenceIds: ["E7"], data: "x", scope });
  assert.ok(!outside.ok && outside.foul === "unsupported_attribution", "束に無い id は通らない");
  const none = checkAttribution({ claimKind: "sourceQuote", evidenceIds: [], data: "x", scope });
  assert.ok(!none.ok, "id 無しの本人発言は通らない");
  const quote = checkAttribution({ claimKind: "sourceQuote", evidenceIds: ["E1"], data: "本人は「毎日の周回が義務になる」と言っている", scope });
  assert.ok(quote.ok, "原文にある引用は通る");
  const fake = checkAttribution({ claimKind: "sourceQuote", evidenceIds: ["E1"], data: "本人は「課金は悪だ」と言っている", scope });
  assert.ok(!fake.ok && fake.foul === "quote_mismatch", "原文に無い引用は通らない");
  const inference = checkAttribution({ claimKind: null, evidenceIds: ["E9"], data: "たぶん好き", scope });
  assert.ok(inference.ok && inference.claimKind === "simulationInference", "未申告は推測として扱う");
  assert.deepEqual(inference.ok ? inference.evidenceIds : null, [], "推測でも束に無い id は落とす");
  assert.equal(attributionSuffix("groundedParaphrase", ["E1", "E2"]), " [E1,E2]");
  assert.equal(attributionSuffix("simulationInference", []), "（推測）");
  assert.ok(!quoteAppearsInEvidence("「ab」", ["abc"]), "短すぎる引用は根拠にしない");
  console.log("ok proxy attribution");
}

// ── 立場 ────────────────────────────────────────────────────────────────────
{
  assert.equal(parseProxyStance("con"), "con");
  assert.equal(parseProxyStance("答え: neutral"), "neutral");
  assert.equal(parseProxyStance("わからない"), null);
  const opposed = assignIssueSides(["con", "pro"], ["A", "B"]);
  assert.deepEqual(opposed, { opposed: true, pro: 1, con: 0 });
  const same = assignIssueSides(["pro", "pro"], ["A", "B"]);
  assert.ok(!same.opposed && same.reason.includes("対立していません"), "同じ側なら反対役を作らない");
  assert.ok(!assignIssueSides(["neutral", "con"], ["A", "B"]).opposed);
  console.log("ok proxy stance");
}

// ── 指定の読み取りとキャスト ────────────────────────────────────────────────
{
  const labelA = proxyLabel(makePack("a1").speakerKey as string);
  const labelB = proxyLabel(makePack("b2").speakerKey as string);
  const req = parseProxyRequest(`代理: ${labelA}, ${labelB} 周回報酬は必要か`)!;
  assert.deepEqual(req.labels, [labelA, labelB]);
  assert.equal(req.theme, "周回報酬は必要か");
  assert.equal(parseProxyRequest("ふつうの議題"), null);
  const withGenre = parseProxyRequest(`周回 代理：${labelA} vs ${labelB} ジャンル: roguelike`)!;
  assert.equal(withGenre.genre, "roguelike");
  assert.equal(withGenre.labels.length, 2);

  const base = { now: NOW, budgetTokens: 3500, models: ["m1", "m2"], isLocal: false, focusTerms: [] as string[] };
  const cast = loadProxyParticipants({ ...base, request: req, requirePassed: false });
  assert.equal(cast.length, 2);
  assert.ok(cast.every((p) => p.role === "debater" && p.proxy && p.possession?.label === "AI代理"));
  assert.deepEqual(cast.map((p) => p.model), ["m1", "m2"]);
  assert.throws(() => loadProxyParticipants({ ...base, request: req, requirePassed: true }), ProxyRequestError, "評価前は出さない (requirePassed)");
  assert.throws(() => loadProxyParticipants({ ...base, now: NOW + 60 * DAY, request: req, requirePassed: false }), /有効期限/);
  assert.throws(() => loadProxyParticipants({ ...base, request: { ...req, labels: [labelA] }, requirePassed: false }), /2 人/);
  assert.throws(() => loadProxyParticipants({ ...base, request: { ...req, labels: [labelA, "論者#ffffff"] }, requirePassed: false }), /取り込まれていません/);

  assert.equal(resolveProxyRun("x", []), null, "代理タグが無ければ何もしない");
  assert.throws(() => resolveProxyRun(`代理: ${labelA}, ${labelB} x`, ["代理", "機密"]), /機密/);
  assert.throws(() => resolveProxyRun("代理の指定なし", ["代理"]), /代理: 論者/);
  const run = resolveProxyRun(`代理: ${labelA}, ${labelB} 周回報酬は必要か`, ["代理"], NOW)!;
  assert.equal(run.theme, "周回報酬は必要か");
  assert.deepEqual(run.participants.map((p) => p.model), ["claude-a@high", "claude-b@high"], "roster の議論者モデルを使う");
  console.log("ok proxy cast");
}

// ── 形成 ────────────────────────────────────────────────────────────────────
{
  const reviews = [
    ...["g1", "g2", "g3", "g4", "g5", "g6"].map((g, i) => ({
      gameSlug: g,
      gameTitle: g.toUpperCase(),
      text: i % 2 === 0 ? `難易度が最高で楽しい。ストーリーも良い。${g}` : `値段が高すぎて遊べる量が少ない。動作も重い。${g}`,
      recommended: i % 2 === 0,
      sourceKind: "steam",
    })),
    ...["g1", "g2", "g3", "g4", "g5", "g6"].map((g) => ({
      gameSlug: g,
      gameTitle: g.toUpperCase(),
      text: `もう一度遊びたい。リプレイ性が高い難易度。見た目もきれい。https://example.com/${g}`,
      recommended: true,
      sourceKind: "steam",
    })),
  ];
  const a = buildPersonaPack({ speakerKey: "ext:steam:0000000000000abc", genre: "roguelike", genreSpecVersion: 1, reviews, now: NOW })!;
  const b = buildPersonaPack({ speakerKey: "ext:steam:0000000000000abc", genre: "roguelike", genreSpecVersion: 1, reviews: [...reviews].reverse(), now: NOW })!;
  assert.deepEqual(a.pack, b.pack, "入力の順に依らず同じパック");
  assert.ok(parsePersonaPack(a.pack).ok, `形成したパックは契約に通る: ${JSON.stringify(!parsePersonaPack(a.pack).ok && (parsePersonaPack(a.pack) as any).errors)}`);
  assert.equal(pickHeldOutGames("k", ["a", "b", "c"]).size, 0, "3 本以下は伏せない");
  assert.equal(a.pack.heldOut.length, 1, "6 本なら 1 本伏せる");
  const heldGame = a.pack.heldOut[0].game;
  assert.ok(!a.pack.evidence.some((e) => e.gameTitle === heldGame), "伏せたゲームは根拠に入れない");
  assert.ok(a.heldOutTexts.length > 0 && a.heldOutTexts.every((h) => h.game === heldGame), "伏せた発言は評価用に別出し");
  assert.ok(!a.pack.evidence.some((e) => e.text.includes("http")), "本文のリンクを伏せる");
  assert.equal(sanitizeReviewText("見て @someone_x さん"), "見て (ID省略) さん");
  assert.ok(a.pack.stanceCards.every((c) => c.extractor === "rule" && c.evidenceIds.length === 1));
  assert.ok(a.pack.stanceCards.every((c) => a.pack.evidence.find((e) => e.id === c.evidenceIds[0])!.text.includes(c.claim.slice(0, 8))), "立場カードは根拠の文そのもの");
  const onlyPos = buildPersonaPack({ speakerKey: "ext:steam:0000000000000abd", genre: "roguelike", genreSpecVersion: 1, reviews: reviews.filter((r) => r.recommended), now: NOW })!;
  assert.equal(onlyPos.pack.designGap.vector, null, "否定的が足りなければ gap を作らない");
  assert.ok(onlyPos.pack.designGap.reason);

  const dict = parseGenreDictionary({ specVersion: 1, games: { "steam-app-1": ["roguelike"] }, titles: { "steam-app-1": "One" } });
  assert.throws(() => parseGenreDictionary({ specVersion: 1, games: { x: ["Bad Genre"] } }));
  const utterances = [
    { source: "steam", nativeId: "1", gameSlug: "steam-app-1", threadKey: "1", content: "難易度が最高で楽しいゲームだった。", postedAt: 0, authorId: "76561198000000001", signal: { votedUp: true } },
    { source: "steam", nativeId: "2", gameSlug: "steam-app-2", threadKey: "2", content: "辞書に無いゲームのレビューです。長さは足りる。", postedAt: 0, authorId: "76561198000000001", signal: { votedUp: true } },
    { source: "steam", nativeId: "3", gameSlug: "steam-app-1", threadKey: "1", content: "短い", postedAt: 0, authorId: "76561198000000001", signal: { votedUp: false } },
  ] as any[];
  const formed = formPersonaPacks({ utterances, dictionary: dict, hmacKey: "test-key", now: NOW });
  assert.equal(formed.report.skipped.unclassified, 1);
  assert.equal(formed.report.skipped.tooShort, 1);
  assert.equal(formed.packs.length, 1);
  assert.equal(formed.packs[0].sufficiency.confidence, "low", "材料が少なければ low");
  assert.ok(!JSON.stringify(formed.packs).includes("76561198000000001"), "生の ID をパックに出さない");
  assert.equal(speakerKeyOf("steam", "76561198000000001", "test-key"), formed.packs[0].speakerKey);
  assert.throws(() => speakerKeyOf("steam", "x", ""), /HMAC/);
  console.log("ok proxy formation");
}

// ── 評価の合否 ──────────────────────────────────────────────────────────────
{
  const score = scoreRecommendations([true, false, true, false], ["yes", "no", "yes", "unknown"], { positive: 5, negative: 5 });
  assert.equal(score.accuracy, 0.75);
  assert.equal(score.abstained, 1);
  assert.equal(score.majorityAccuracy, 0.5);
  const pass = judgeProxyEval({ recommendation: score, discrimination: 0.8, groundedRate: 0.9, misattributionRate: 0 });
  assert.equal(pass.status, "passed");
  const allPos = scoreRecommendations([true, true, true], ["yes", "yes", "yes"], { positive: 9, negative: 1 });
  assert.equal(judgeProxyEval({ recommendation: allPos, discrimination: null, groundedRate: null, misattributionRate: null }).status, "unevaluated", "3 本未満は判定しない");
  const allPos4 = scoreRecommendations([true, true, true, true], ["yes", "yes", "yes", "yes"], { positive: 9, negative: 1 });
  const v = judgeProxyEval({ recommendation: allPos4, discrimination: null, groundedRate: null, misattributionRate: null });
  assert.equal(v.status, "failed", "多数派と同じ当たり方では合格しない");
  const misattr = judgeProxyEval({ recommendation: score, discrimination: null, groundedRate: 0.9, misattributionRate: 0.2 });
  assert.equal(misattr.status, "failed");
  console.log("ok proxy eval verdict");
}

// ── dialectic 代理モード E2E ───────────────────────────────────────────────
{
  const labelA = proxyLabel(makePack("a1").speakerKey as string);
  const labelB = proxyLabel(makePack("b2").speakerKey as string);
  const participants = loadProxyParticipants({
    request: { labels: [labelA, labelB], genre: null, theme: "周回報酬" },
    now: NOW,
    requirePassed: false,
    budgetTokens: 3500,
    models: ["claude-a@high", "claude-b@high"],
    isLocal: false,
    focusTerms: [],
  });
  const who = (p: string) => (p.includes(`代理する人物: ${labelA}`) ? "A" : p.includes(`代理する人物: ${labelB}`) ? "B" : "?");
  const stats: Record<string, number> = {};
  const llm = {
    async invoke(args: { prompt: string }) {
      const p = args.prompt;
      const hit = (name: string, text: string) => {
        stats[name] = (stats[name] ?? 0) + 1;
        return { ok: true as const, text };
      };
      if (p.includes("この人物がこの論点に賛成するか反対するか")) {
        // 論点 1: A=賛成 / B=反対、論点 2: 両方賛成 (対立なし)
        if (p.includes("論点その1")) return hit("proxy-stance", who(p) === "A" ? "pro" : "con");
        return hit("proxy-stance", "pro");
      }
      if (p.includes("定立 (Position) を張ります")) {
        return hit(
          "position",
          JSON.stringify({
            claim: p.includes("(pro)") ? "賛成の主張" : "反対の主張",
            grounds: [
              { kind: "induction", data: "日課の義務は嫌だと言っている", warrant: "義務化は続ける気を削ぐ", say: "義務になるのは嫌なんだ。", claimKind: "groundedParaphrase", evidenceIds: ["E1"] },
              { kind: "abduction", data: "本人は「課金は悪」と言っている", warrant: "x", say: "課金も嫌だね。", claimKind: "sourceQuote", evidenceIds: ["E2"] },
            ],
            values: ["体験"],
            text: p.includes("(pro)") ? "賛成だ。" : "反対だ。",
          })
        );
      }
      if (p.includes("あなたは議論の進行役です。次の進行内容")) return hit("facilitator", "では続けましょう。");
      if (p.includes("rebut | concede") && p.includes("最後の論拠に答えて")) {
        // 束に無い根拠 E9 を本人の発言として引く反論 → 問いに格下げされる
        return hit(
          "turn",
          JSON.stringify({ act: "rebut", attack: "data", kind: "induction", data: "別のデータ", warrant: "崩れる", text: "それは違うと思う。", claimKind: "groundedParaphrase", evidenceIds: ["E9"] })
        );
      }
      if (p.includes("rebut | concede")) return hit("turn-other", JSON.stringify({ act: "concede", text: "認める。" }));
      if (p.includes("前提ルールを守っているかを判定")) return hit("rule-check", "ok");
      if (p.includes("「はい」と答えているか「いいえ」と答えているか")) return hit("position-stance", p.includes("賛成の主張") ? "yes" : "no");
      if (p.includes("返答は相手の発言を認めているか")) return hit("concession", "concede");
      if (p.includes("この対立の「型」を 1 つだけ選んで")) return hit("tension", "values");
      if (p.includes("止揚 (aufheben) の 3 要件")) return hit("synth", JSON.stringify({ text: "条件で分ける", preserves: [], negates: [], elevates: "" }));
      if (p.includes("統合案の「新しい枠」")) return hit("gate", "compromise");
      if (p.includes("チェックリスト (敵対的批准")) return hit("ratify", JSON.stringify({ verdict: "reject", preservedIds: [], missing: [], reason: "x" }));
      if (p.includes("あなたは中立な審判として")) {
        const ids = [...p.matchAll(/\[ID:([^\]]+)\]/g)].map((m) => m[1]).sort();
        return hit("vote", ids[0] ?? "");
      }
      if (p.includes('"summary"')) return hit("conclusion", '{"summary": "まとめ"}');
      if (p.includes("重視する価値 (価値軸)")) return hit("persona-setup", "[]");
      return hit("unmatched", "(unmatched)");
    },
  };

  const result = await runDialecticFlow("周回報酬は必要か", ["代理"], {
    llm,
    gamesDir: path.join(TMP_DIR, "no-games-dir"),
    sessionId: "proxy-e2e",
    paperOverride: { mechanics: [], supplement: "", issues: ["論点その1は妥当か?", "論点その2は妥当か?"] },
    proxyParticipants: participants,
    log: () => {},
    warn: () => {},
  });
  assert.equal(stats.unmatched ?? 0, 0, "未ルーティングの LLM 呼び出しなし");
  assert.equal(stats["proxy-stance"], 4, "論点 2 件 × 代理 2 人の立場判定");
  assert.equal(result.rounds, 2);

  const d = new Database(DB_PATH);
  const issues = d.prepare("SELECT * FROM flow_issue WHERE session_id = ? ORDER BY ordinal").all("proxy-e2e") as any[];
  const pos1 = d.prepare("SELECT * FROM flow_position WHERE issue_id = ?").all(issues[0].id) as any[];
  assert.equal(pos1.length, 2, "対立した論点は定立 2 件");
  const proPos = pos1.find((p) => p.stance === "pro");
  assert.equal(proPos.persona_id, participants[0].id, "本人のデータで賛成と判定された A が賛成側");
  const grounds = JSON.parse(proPos.grounds_json);
  assert.equal(grounds[0].claimKind, "groundedParaphrase", "束にある根拠の言い換えは本人の発言として残る");
  assert.equal(grounds[1].claimKind, "simulationInference", "原文に無い引用は推測に落とす");
  const pos2 = d.prepare("SELECT * FROM flow_position WHERE issue_id = ?").all(issues[1].id) as any[];
  assert.equal(pos2.length, 0, "対立しない論点は定立を作らない (反対役を作らない)");

  const utterances = d.prepare("SELECT * FROM flow_utterance WHERE session_id = ? ORDER BY created_at, turn").all("proxy-e2e") as any[];
  assert.ok(utterances[0].text.includes("AI の代理"), "冒頭で AI の代理であることを伝える");
  const groundedUtterance = utterances.find((u) => u.claim_kind === "groundedParaphrase");
  assert.ok(groundedUtterance && JSON.parse(groundedUtterance.evidence_ids_json)[0] === "E1" && groundedUtterance.text.includes("[E1]"), "帰属区分・根拠 id・露出の印が残る");
  assert.ok(utterances.some((u) => u.claim_kind === "simulationInference" && u.text.includes("（推測）")), "推測は露出で区別");
  assert.ok(!utterances.some((u) => u.act === "rebut"), "束に無い根拠の反論は論拠にならない");
  assert.ok(utterances.some((u) => u.act === "question" && u.persona_id.startsWith("proxy:")), "問いに格下げされる");
  assert.ok(utterances.some((u) => u.text.includes("この論点では対立していません")), "対立なしの理由を流す");
  assert.ok(utterances.filter((u) => u.persona_id.startsWith("proxy:")).every((u) => u.possession_name === "AI代理"), "露出名に AI 代理");
  const sessionPersonas = d.prepare("SELECT * FROM flow_session_persona WHERE session_id = ?").all("proxy-e2e") as any[];
  assert.ok(sessionPersonas.filter((p) => p.id.startsWith("proxy:")).every((p) => p.value_axis === null && p.core_claims_json === null), "代理に価値軸・核主張を作らない");
  d.close();
  console.log("ok proxy dialectic e2e");
}
