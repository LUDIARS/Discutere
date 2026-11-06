// flow の純関数 (w1-E 担当分) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-E-flow_pure.gen.ts → native/tests/fixtures/flow/w1e_pure.json
import fs from "node:fs";
import path from "node:path";
import { splitBlocks, joinBlocks, replaceBlock, insertBlockAfter, getBlockText } from "../../../src/flow/paper-blocks.js";
import { classifyPaperReviewIntent, isNonInstructionReply } from "../../../src/flow/paper-review-intent.js";
import { detectSparringCommand } from "../../../src/flow/sparring-commands.js";
import { composeDisplayName } from "../../../src/flow/persona-display.js";
import { assessConvergence } from "../../../src/flow/dialectic/convergence.js";
import { PREMISE_RULES_TEXT, coerceAttackPoint, coerceInferenceKind } from "../../../src/flow/dialectic/premise-rules.js";
import { isRedoText, decideRedo } from "../../../src/flow/discussion-redo.js";
import { canCollectExternal, paperSupplement, wantsForesight, wantsProxy } from "../../../src/flow/tags.js";
import { parsePersonaPack } from "../../../src/flow/proxy/contract.js";
import { checkAttribution, coerceEvidenceIds, coerceClaimKind, quoteAppearsInEvidence, attributionSuffix } from "../../../src/flow/proxy/attribution.js";
import { proxyPromptLines, proxyGroundFields } from "../../../src/flow/proxy/prompt.js";
import { parsePersonaDocument } from "../../../src/flow/persona-import-document.js";
import { generateFlowPersonas, personaStance, rosterPersonaCount } from "../../../src/flow/personas.js";
import { buildTurnOrder, shuffle } from "../../../src/flow/turn-scheduler.js";
import { ArgumentGraph } from "../../../src/flow/argument-graph.js";

function lcg(seedInit: number) {
  let seed = seedInit;
  return () => {
    seed = (seed * 1664525 + 1013904223) & 0xffffffff;
    return (seed >>> 0) / 0x100000000;
  };
}

const mds = [
  "",
  "# 議題\n本文1\n本文2\n\n- a\n- b\n1. c\n2) d\n段落\n## 見出し2\n\n\n末尾  ",
  "####### 7つ\n#タグ\n#\tタブ見出し\n  * 字下げ箇条\n+ plus\n-ダッシュだけ\n10. ten\n3.no-space\r\n\r\nwin\r\nlines   \n　\n全角空白行の後",
  "line\rcr\nnext",
];
const blocks = mds.map((md) => {
  const b = splitBlocks(md);
  return {
    md,
    blocks: b,
    joined: joinBlocks(b),
    replaced: [replaceBlock(md, "b0", "置換\n\n  "), replaceBlock(md, "b1", "  "), replaceBlock(md, "b99", "x")],
    inserted: [insertBlockAfter(md, "b0", "挿入  "), insertBlockAfter(md, "zz", "末尾に"), insertBlockAfter(md, "b0", " ")],
    got: [getBlockText(md, "b1"), getBlockText(md, "b42")],
  };
});

const intentTexts = [
  "メカニクスの代替として モンスト、パズドラ を参考に", "ユーザーの感想を集めて", "口コミも入れて", "外部の声を取り込んで「周回」",
  "外部の声を取り込んで「周回」「スタミナ」", "観点補足を初心者向けに", "類似ゲーム: 「モンスト」「パズドラ」", "類似ゲームの外部の声を集めて",
  "似たようなゲーム：スプラトゥーンとフォートナイト、Apex Legends!", "参考になるゲームはポケモンとドラクエです。", "代替メカニクスを追加してください",
  "  類似する作品 「」 『FF』 \"DQ\" “MH” ", "レビューを拾って", "コメント追加", "反応入れて", "類似のタイトル:ゼルダ/マリオ／カービィ・メトロイド",
  "似ているゲーム として と ことと", "参考作品: とある魔術,abc", "類似ゲーム", "", "類似ゲーム: 「ab", "似たゲーム 「x」",
];
const replyTexts = [
  "ない", "ない。", " 「ない」 ", "なし!!", "OK", "Yes", "了解ー", "😀😀", "!?", "a", "ab", "はい〜", "大丈夫です。", "ないです",
  "メカニクスを追加", "特にない", "ⅰⅱ", "١٢", "「」", "』』", "  ", "ガチャ", "ＮＯ", "No.",
];
const sparring = [
  "おわり", "このメカニクスはまとめて評価すると面白いが、難易度が高すぎて序盤で飽きると思う", "そろそろまとめて。", "まだ終わらないでほしい",
  "まとめて", "まとめてください", "もう終了で", "コンボが楽しい", "終わりにして", "", "  。。 ", "終わりたくない", "おわれない",
  "長い長い長い長い長い長い長い長い意見ですがここで終了します", "まとめの観点から長い長い長い長い長い長い話をする", "途中にまとめが入る長い長い長い長い文章です",
  "「おしまい」", "終了 ! ", "まとめておわり",
];
const display = [
  { name: "陽介", stance: "opinion" }, { name: "凛", stance: "pro" }, { name: "葵", role: "debater" },
  { name: "蓮", stance: "opinion", possessionName: "  " }, { name: "陽介", stance: "opinion", possessionName: "論者#a1b2c3" },
  { name: "翔" }, { name: "改", stance: "reform", possessionName: " x " }, { name: "保", stance: "conservative" },
  { name: "未知", stance: "weird", role: "debater" }, { name: "空", stance: "", role: "facilitator" },
] as const;
const conv = [
  { unresolvedRebuttals: 0, winnerShare: 0.7, convergeShare: 0.6, newClaimsInWindow: 0, ratifiedAufhebenCount: 0, aufhebungTarget: 3 },
  { unresolvedRebuttals: 2, winnerShare: null, convergeShare: 0.6, newClaimsInWindow: 1, ratifiedAufhebenCount: 3, aufhebungTarget: 3 },
  { unresolvedRebuttals: 1, winnerShare: 0.555, convergeShare: 0.55, newClaimsInWindow: 0, ratifiedAufhebenCount: 1, aufhebungTarget: 0 },
  { unresolvedRebuttals: 0, winnerShare: 0.125, convergeShare: 0.6, newClaimsInWindow: 4, ratifiedAufhebenCount: 0, aufhebungTarget: 2 },
  { unresolvedRebuttals: 3, winnerShare: 0.005, convergeShare: 0.9, newClaimsInWindow: 2, ratifiedAufhebenCount: 0, aufhebungTarget: 1 },
];
const coerceIn = ["deduction", " Induction ", "演繹", "帰納", "アブダクション", "DATA", "論拠", "限定", "x", 3, null, "warrant", "qualifier"];
const tagSets = [[], ["開発"], ["運用", "開発", "内部", "機密"], ["未来予測", "代理"], ["機密"], ["内部"], ["その他"]];

const NOW = 1_800_000_000_000;
const DAY = 86400000;
const vec = (v = 0.5) => new Array(20).fill(v);
function makePack(over: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    contractVersion: 1, personaKind: "observedSpeaker", speakerKey: "ext:steam:00000000000000a1", genre: "roguelike",
    genreSpecVersion: 1, profileVersion: "proxy-form-v1:a1", formation: { method: "rule", ruleVersion: "proxy-form-v1" },
    window: { observedMonths: null, generatedAt: NOW - DAY, expiresAt: NOW + 30 * DAY },
    sufficiency: { utterances: 12, positive: 7, negative: 5, games: 4, aspects: 4, confidence: "medium", reasons: [] },
    affect: { vector: vec(0.6), vectorSpecVersion: 1 },
    designGap: { vector: vec(0.1), method: "pos-minus-neg", wants: ["asp.difficulty"], complaints: ["asp.content"], reason: null },
    conditionalGaps: [{ current: "今", desired: "望み", condition: null, evidenceIds: ["E1"] }],
    stanceCards: [
      { id: "S1", claim: "毎日の周回を強いる作りは嫌だ", polarity: "neg", evidenceIds: ["E1"], extractor: "rule" },
      { id: "S2", claim: "高難度に何時間でも挑みたい", polarity: "pos", evidenceIds: ["E2"], extractor: "llm" },
    ],
    evidence: [
      { id: "E1", revision: "r1", text: "  毎日の周回が義務になるのは本当に嫌だ。 ", sourceKind: "steam", gameTitle: "Game A", polarity: "neg", postedAt: null },
      { id: "E2", revision: "r2", text: "週末なら高難度に何時間でも挑みたい。", sourceKind: "steam", gameTitle: "Game B", polarity: "pos", postedAt: 1700000000000 },
    ],
    styleNotes: ["短く書く"], likedGames: ["Game B"],
    heldOut: [{ game: "Game D", recommended: true }, { game: "Game E", recommended: false }],
    revoked: false,
    ...over,
  };
}
const packCases: unknown[] = [
  makePack(),
  makePack({ speakerKey: "ext:steam:76561198000000000" }),
  makePack({ evidence: [{ id: "E1", revision: "r", text: "see https://x", sourceKind: "s", gameTitle: "g", polarity: "neg", sourceUrl: "HTTP://y" }] }),
  makePack({ stanceCards: [{ id: "S1", claim: "x の主張です", polarity: "pos", evidenceIds: ["E9"], extractor: "rule" }] }),
  makePack({ stanceCards: [{ id: "T1", claim: "", polarity: "zz", evidenceIds: [], extractor: "x" }] }),
  makePack({ authorName: "someone", nested: { user_id: 1, list: [{ url: "a" }] } }),
  makePack({ contractVersion: 2, personaKind: "x", genre: "Bad Genre", genreSpecVersion: -1, profileVersion: "a".repeat(129) }),
  makePack({ window: { observedMonths: 1.5, generatedAt: 5, expiresAt: 5 }, affect: { vector: [1, 2], vectorSpecVersion: 2 }, revoked: "no" }),
  makePack({ designGap: { vector: null, method: "x", wants: "nope", complaints: ["a".repeat(41)], reason: null } }),
  makePack({ designGap: { vector: null, method: "pos-minus-neg", reason: "データ不足" }, sufficiency: "x" }),
  makePack({ evidence: Array.from({ length: 13 }, (_, i) => ({ id: `E${i % 3}`, revision: "r", text: "t", sourceKind: "s", gameTitle: "g", polarity: "pos", postedAt: "x" })) }),
  makePack({ evidence: [] , stanceCards: "x", conditionalGaps: 3, heldOut: [{ game: "g" }, 5], styleNotes: ["a", "b", "c", "d", "e"], likedGames: [1] }),
  makePack({ formation: "rule", window: null, sufficiency: { utterances: 1, positive: "2", negative: -1, games: 0.5, aspects: 0, confidence: "low", reasons: ["r", ""] } }),
  makePack({ speakerKey: "ext:Steam:00000000000000a1" }),
  makePack({ speakerKey: "ext:s-t_e:0123456789abcdef" }),
  makePack({ genre: "a".repeat(64) }),
  makePack({ genre: "a".repeat(65) }),
  [1, 2],
  "str",
];
const packs = packCases.map((raw) => ({ raw, result: parsePersonaPack(raw) }));

const scope = { evidenceIds: ["E1", "E2"], evidenceTexts: { E1: "毎日の周回が 義務になるのは本当に嫌だ。", E2: "週末なら高難度に挑みたい。" } };
const attribution = [
  { claimKind: "groundedParaphrase", evidenceIds: ["E1"], data: "日課の義務は嫌" },
  { claimKind: "groundedParaphrase", evidenceIds: ["E7"], data: "x" },
  { claimKind: "sourceQuote", evidenceIds: [], data: "x" },
  { claimKind: "sourceQuote", evidenceIds: ["E1"], data: "本人は「毎日の周回が義務になる」と言っている" },
  { claimKind: "sourceQuote", evidenceIds: ["E1"], data: "本人は「課金は悪だ」と言っている" },
  { claimKind: null, evidenceIds: ["E9", "E2"], data: "たぶん好き" },
  { claimKind: "unknown", evidenceIds: [], data: "?" },
  { claimKind: "sourceQuote", evidenceIds: ["E1", "E2"], data: "毎日の周回が義務" },
  { claimKind: "sourceQuote", evidenceIds: ["E2"], data: "「ab」「週末なら 高難度」" },
].map((args) => ({ args, result: checkAttribution({ ...args, claimKind: args.claimKind as never, scope }) }));
const evidenceIdInputs = [["E1", " [E2] ", "E1", "E1000", "e3", "[E4", "E5]", 7, "E", "E999"], "x", null];
const quoteCases = [["「ab」", ["abc"]], ["「abcd」", ["xxabcdxx"]], ["\"a b c d\"", ["abcd"]], ["no quote here", ["no quote here!"]], ["「abc」と『defg』", ["defgh"]], ["“1234”", ["12 34"]]];

const proxyPersona = { id: "p", name: "n", role: "debater", stance: "pro", speechStyle: "", traits: [], model: "m", isLocal: false,
  proxy: { label: "論者#aaaaaa", speakerKey: "ext:steam:0000000000000001", genre: "g", bundleText: "# 束\n本文", evidenceIds: ["E1"], evidenceTexts: { E1: "t" } } };

const docs = ["", "  [1, {\"a\":2}]  ", "{\"personas\":[{\"x\":1}]}", "{\"single\":true}", "\"str\"", "{\"a\":1}\n{bad}\r\n\n{\"b\":2}\n[3]", "x\ny"];

const personaRuns = [
  { count: 1, rngSeed: 1 }, { count: 2, rngSeed: 2 }, { count: 5, rngSeed: 42 }, { count: 6, rngSeed: 7 }, { count: 9, rngSeed: 3 },
  { count: 25, rngSeed: 11 },
  { count: 4, rngSeed: 5, roster: { facilitator: "fac", discussants: ["a", "b"] } },
  { count: 3, rngSeed: 5, roster: { facilitator: "", discussants: [] } },
].map((args) => {
  const ps = generateFlowPersonas({ count: args.count, defaultModel: "def", isLocal: args.count % 2 === 0, rng: lcg(args.rngSeed), roster: args.roster });
  return { args, personas: ps.map(({ id: _id, ...rest }) => rest), stances: ps.map(personaStance) };
});
const order = (() => {
  const ps = generateFlowPersonas({ count: 6, defaultModel: "m", isLocal: false, rng: lcg(9) }).map((p, i) => ({ ...p, id: `p${i}` }));
  return [0, 1, 3, 5, 12].map((turns) => ({ turns, ids: buildTurnOrder(ps, turns, lcg(turns + 100)).map((p) => p.id) }));
})();
const shuffled = shuffle([1, 2, 3, 4, 5, 6, 7], lcg(77));

const graphOps = [
  ["c1", "pro", "claim", null, 1], ["c2", "con", "claim", null, 2], ["r1", "con", "rebut", "c1", 3], ["r2", "op", "rebut", "c1", 4],
  ["rr", "pro", "rebut", "r1", 5], ["s1", "pro", "support", "r2", 6], ["s2", "con", "support", "c2", 7], ["q1", "op", "question", "c2", 8],
  ["x1", "pro", "concede", "rr", 9], ["c3", "op", "synthesize", "c1", 10], ["r3", "pro", "rebut", "c3", 11], ["x2", "op", "concede", "c3", 12],
  ["r4", "x", "rebut", null, 13], ["r5", "x", "rebut", "nowhere", 14], ["c4", "x", "claim", null, 15],
] as const;
const g = new ArgumentGraph();
for (const [id, personaId, act, targetId, turn] of graphOps) g.apply({ id, personaId, act, targetId, turn } as never);
const graph = {
  ops: graphOps,
  unresolved: g.unresolvedRebuttals(), unanswered: g.unansweredClaims(), concessions: g.concessions(),
  since: [0, 5, 10, 15].map((t) => g.newClaimsSince(t)), nodes: g.claimNodes(),
  edgeCounts: ["c1", "c2", "c3", "r1"].map((c) => g.edgeCountFor(c)), targeting: g.rebuttalsTargeting("c1"),
  byId: [g.rebuttalById("rr") ?? null, g.rebuttalById("zz") ?? null],
};

const out = {
  blocks,
  intents: intentTexts.map((t) => ({ text: t, result: classifyPaperReviewIntent(t) })),
  replies: replyTexts.map((t) => ({ text: t, result: isNonInstructionReply(t) })),
  sparring: sparring.map((t) => ({ text: t, result: detectSparringCommand(t) })),
  display: display.map((input) => ({ input, result: composeDisplayName(input as never) })),
  convergence: conv.map((input) => ({ input, result: assessConvergence(input) })),
  premise: { text: PREMISE_RULES_TEXT, inference: coerceIn.map((v) => coerceInferenceKind(v) ?? null), attack: coerceIn.map((v) => coerceAttackPoint(v) ?? null), inputs: coerceIn },
  redo: {
    texts: ["再開", "  やり直してください", "もう一度", "もういちど", "再議論", "やりなおし", "いいえ再開", ""].map((t) => ({ t, r: isRedoText(t) })),
    decisions: [
      { paper: null, concluded: false, running: false },
      { paper: { status: "draft", flow: "discussion" }, concluded: false, running: false },
      { paper: { status: "started", flow: "sparring" }, concluded: false, running: false },
      { paper: { status: "started", flow: "improvement" }, concluded: true, running: false },
      { paper: { status: "started", flow: "discussion" }, concluded: false, running: true },
      { paper: { status: "started", flow: "discussion" }, concluded: false, running: false },
    ].map((input) => ({ input, result: decideRedo(input) })),
  },
  tags: tagSets.map((tags) => ({ tags, can: canCollectExternal(tags), supplement: paperSupplement(tags), foresight: wantsForesight(tags), proxy: wantsProxy(tags) })),
  packs,
  attribution: { scope, cases: attribution },
  evidenceIds: evidenceIdInputs.map((v) => ({ v, r: coerceEvidenceIds(v) })),
  claimKinds: ["sourceQuote", "unknown", "x", 1].map((v) => ({ v, r: coerceClaimKind(v) ?? null })),
  quotes: quoteCases.map(([d, c]) => ({ d, c, r: quoteAppearsInEvidence(d as string, c as string[]) })),
  suffix: [["groundedParaphrase", ["E1", "E2"]], ["sourceQuote", []], ["simulationInference", []], ["unknown", ["E1"]]].map(([k, ids]) => ({ k, ids, r: attributionSuffix(k as never, ids as string[]) })),
  proxyPrompt: { persona: proxyPersona, lines: proxyPromptLines(proxyPersona as never), fields: proxyGroundFields(proxyPersona as never), plainLines: proxyPromptLines({ ...proxyPersona, proxy: undefined } as never), plainFields: proxyGroundFields({ ...proxyPersona, proxy: undefined } as never) },
  docs: docs.map((d) => ({ d, r: parsePersonaDocument(d) })),
  personas: personaRuns,
  rosterCounts: [rosterPersonaCount(4, { discussants: ["a", "b", "c"] }), rosterPersonaCount(9, undefined), rosterPersonaCount(9, { discussants: [] })],
  order,
  shuffled,
  graph,
};
const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "flow");
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, "w1e_pure.json"), JSON.stringify(out, null, 2) + "\n");
console.log("wrote w1e_pure.json");
