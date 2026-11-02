// w2-H 担当 (ペルソナの母集団・代理議論の束・Notion 読み込み等) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w2-H-flow.gen.ts → native/tests/fixtures/flow/w2h.json
// DB・ネットワーク・LLM には触れない純関数だけを記録する (Notion は公開ページ取得を注入した偽物)。
import fs from "node:fs";
import path from "node:path";
import { buildPersonaDescriptor } from "../../../src/flow/persona-descriptor.js";
import { evaluateSpeakers } from "../../../src/flow/persona-adopt.js";
import { parseVoluptasPersonaPayload } from "../../../src/flow/persona-import.js";
import { parsePersonaPack } from "../../../src/flow/proxy/contract.js";
import { estimateTokens, orderEvidence, renderProxyBundle, dimLabel } from "../../../src/flow/proxy/bundle.js";
import { parseProxyRequest } from "../../../src/flow/proxy/cast.js";
import { parseProxyStance, buildProxyStancePrompt, assignIssueSides } from "../../../src/flow/proxy/stance.js";
import { negativeBaselineVector, buildTargetVector, computeDesignGap } from "../../../src/flow/design-gap.js";
import { parseNotionDepth, expandNotionLinks } from "../../../src/flow/notion-link.js";
import { findNotionUrls, isNotionUrl } from "@ludiars/canalis";
import { voicePreparationNotice, type VoicePreparation } from "../../../src/flow/user-voices/preparation.js";
import { fallbackDomainsToMechanics, refineDomainsToMechanics } from "../../../src/flow/anatomia/refine.js";
import { MockLLMClient } from "../../../src/persona-engine/llm/mock.js";

const out: Record<string, unknown> = {};

// ── persona descriptor ─────────────────────────────────────────────
const base = { id: "p", name: "n", role: "opinion" as const, speechStyle: "", traits: [] as string[], affectVector: new Array(20).fill(0.2), origin: "imported" as const, parentIds: [] as string[] };
const personas = [
  { ...base, preferenceAxes: { "style.explorer": 0.9, "style.narrative": 0.8, "style.routine_tolerance": 0.1 }, attributes: { ageBand: "20s", spending: "light" }, aversions: [{ target: "mechanic:core/gacha", strength: 0.8 }], mechanicReactions: [{ mechanicId: "action/dodge-roll", sentiment: 1.5 }] },
  { ...base, preferenceAxes: { "style.b_x": 0.5, "style.bx": 0.5, "mtg.Z": 0.5, "unknown.axis": 0.7 } },
  { ...base, preferenceAxes: { "style.explorer": 0.4 }, aversions: [{ target: "b", strength: 0.5 }, { target: "a", strength: 0.5 }, { target: "mechanic:c", strength: 0.9 }], mechanicReactions: [{ mechanicId: "x", sentiment: -1.8 }, { mechanicId: "y", sentiment: 1.8 }, { mechanicId: "z", sentiment: 0 }] },
  { ...base, attributes: { spending: "heavy" } },
  { ...base, traits: ["高難度志向", "上達快感"], speechStyle: "ストイック", learningSource: "steam", typicality: 0.95, polarityBias: 0.6, affectDispersion: 0.2 },
  { ...base, typicality: 0.5, polarityBias: 0.2, affectDispersion: 0.05 },
  { ...base },
];
out.descriptors = personas.map((p) => ({ persona: p, text: buildPersonaDescriptor(p as never) }));

// ── evaluateSpeakers ───────────────────────────────────────────────
const mk = (n: number, text: string, game: string | null, weight?: number) =>
  Array.from({ length: n }, () => ({ text, gameSlug: game, ...(weight !== undefined ? { weight } : {}) }));
const speakers = [
  { speakerId: "ext:steam:good", source: "steam", opinions: [...mk(6, "great fun awesome", "game-a"), ...mk(6, "boring bad terrible", "game-b")] },
  { speakerId: "ext:steam:allpos", source: "steam", opinions: [...mk(5, "great awesome best", "game-a"), ...mk(5, "fun amazing love", "game-b")] },
  { speakerId: "ext:steam:allneg", opinions: [...mk(5, "boring bad terrible", "game-a"), ...mk(5, "awful worst hate", "game-b")] },
  { speakerId: "ext:steam:few", source: "steam", opinions: mk(5, "good", "game-a") },
  { speakerId: "ext:steam:onegame", source: "steam", opinions: [...mk(5, "great fun", "game-a"), ...mk(5, "bad boring", "game-a")] },
  { speakerId: "ext:yt:weighted", source: "youtube", opinions: [...mk(4, "楽しい 最高 面白い", "g1", 5), ...mk(4, "つまらない 最悪 退屈", "g2", 1), ...mk(4, "普通", null)] },
  { speakerId: "ext:yt:nogap", opinions: [...mk(6, "x", "g1"), ...mk(6, "x", "g2")] },
];
out.evaluateSpeakers = {
  speakers,
  default: evaluateSpeakers(speakers),
  loose: evaluateSpeakers(speakers, { minOpinions: 3, polarityEps: 0.2, gameGapEps: 0 }),
};

// ── parseVoluptasPersonaPayload ────────────────────────────────────
const uid = "ext:voluptas:0123456789abcdef";
const vec = new Array(20).fill(0.25);
const payloadCases: unknown[] = [
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, traits: ["探索型"] },
  { pseudoId: uid, affectVector: vec, vectorSpecVersion: 1, exportSpecVersion: 2, traits: ["  a ", "a", "", "x".repeat(100), "x".repeat(90)], preferenceAxes: { "style.routine_tolerance": 0.1, "style.explorer": 0.9, "mtg.timmy": 0, "style.a_b": 1, "style.ab": 0.5 }, attributes: { ageBand: " 20s ", spending: "" }, aversions: [{ target: " mechanic:core/gacha ", strength: 0.8 }], mechanicReactions: [{ mechanicId: "action/dodge-roll", sentiment: 1.5 }] },
  { pseudoId: uid, affectVector: vec, vectorSpecVersion: 1, traits: Array.from({ length: 40 }, (_, i) => `t${i}`) },
  { pseudoId: uid, vectorSpecVersion: null, vector_spec_version: 1, affectVector: vec },
  { user_id: "raw-voluptas-sid", affect_vector: vec, vector_spec_version: 1 },
  { user_id: uid, affect_vector: [0], vector_spec_version: 1 },
  { user_id: uid, affect_vector: vec, vector_spec_version: 2 },
  { user_id: uid, affect_vector: vec },
  { user_id: uid, affect_vector: vec, vectorSpecVersion: null },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, exportSpecVersion: 1 },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, exportSpecVersion: null },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, exportSpecVersion: "2" },
  { user_id: uid, affect_vector: [...vec.slice(0, 19), 1.5], vector_spec_version: 1 },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, traits: "x" },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, traits: [1] },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, preferenceAxes: { "Style.x": 0.5 } },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, preferenceAxes: [] },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, attributes: { ageBand: "y".repeat(81) } },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, aversions: [{ target: " ", strength: 0.5 }] },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, mechanicReactions: [{ mechanicId: "Bad", sentiment: 0 }] },
  { user_id: uid, affect_vector: vec, vector_spec_version: 1, mechanicReactions: [{ mechanicId: "ok", sentiment: 3 }] },
  [],
  null,
];
out.payloads = payloadCases.map((input) => {
  try {
    return { input, ok: true, payload: parseVoluptasPersonaPayload(input) };
  } catch (e) {
    return { input, ok: false, error: (e as Error).message };
  }
});

// ── proxy bundle ───────────────────────────────────────────────────
const NOW = 1_800_000_000_000;
const DAY = 86_400_000;
function makePack(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    contractVersion: 1, personaKind: "observedSpeaker", speakerKey: "ext:steam:00000000000000a1", genre: "roguelike", genreSpecVersion: 1,
    profileVersion: "proxy-form-v1:a1", formation: { method: "rule", ruleVersion: "proxy-form-v1" },
    window: { observedMonths: null, generatedAt: NOW - DAY, expiresAt: NOW + 30 * DAY },
    sufficiency: { utterances: 12, positive: 7, negative: 5, games: 4, aspects: 4, confidence: "medium", reasons: [] },
    affect: { vector: new Array(20).fill(0.6), vectorSpecVersion: 1 },
    designGap: { vector: new Array(20).fill(0.1), method: "pos-minus-neg", wants: ["asp.difficulty", "emo.joy", "x.unknown"], complaints: ["asp.content"], reason: null },
    conditionalGaps: [{ current: "毎日の周回", desired: "週末にまとめて", condition: "社会人", evidenceIds: ["E1"] }, { current: "a", desired: "b", condition: null, evidenceIds: ["E2", "E3"] }],
    stanceCards: [
      { id: "S1", claim: "毎日の周回を強いる作りは嫌だ", polarity: "neg", evidenceIds: ["E1"], extractor: "rule" },
      { id: "S2", claim: "高難度に何時間でも挑みたい", polarity: "pos", evidenceIds: ["E2", "E10"], extractor: "rule" },
    ],
    evidence: [
      { id: "E1", revision: "r1", text: "毎日の周回が義務になるのは本当に嫌だ。続ける気がなくなった。", sourceKind: "steam", gameTitle: "Game A", polarity: "neg", postedAt: null },
      { id: "E2", revision: "r2", text: "週末なら高難度に何時間でも挑みたい。難易度が最高。", sourceKind: "steam", gameTitle: "Game B", polarity: "pos", postedAt: null },
      { id: "E3", revision: "r3", text: "遊べる量が少なくて値段に見合わない。", sourceKind: "steam", gameTitle: "Game C", polarity: "neg", postedAt: null },
      { id: "E10", revision: "r10", text: "The difficulty curve is great and I keep coming back for more runs.", sourceKind: "steam", gameTitle: "Game D", polarity: "pos", postedAt: null },
      { id: "E4", revision: "r4", text: "周回 周回 周回", sourceKind: "steam", gameTitle: "周回ゲーム", polarity: "pos", postedAt: null },
    ],
    styleNotes: ["短く書く", "語尾が強い"], likedGames: ["Game B"], heldOut: [], revoked: false,
    ...overrides,
  };
}
const parsedPack = parsePersonaPack(makePack());
if (!parsedPack.ok) throw new Error(JSON.stringify(parsedPack.errors));
const packNoStyle = parsePersonaPack(makePack({ styleNotes: [], conditionalGaps: [], designGap: { vector: null, method: "pos-minus-neg", wants: [], complaints: [], reason: "x" } }));
if (!packNoStyle.ok) throw new Error(JSON.stringify(packNoStyle.errors));
const full = renderProxyBundle(parsedPack.pack, { budgetTokens: 100000, label: "論者#aaaaaa", focusTerms: ["周回"] })!;
const budgets = [100000, full.estimatedTokens, full.estimatedTokens - 1, full.estimatedTokens - 60, 400, 300, 50];
out.bundle = {
  pack: makePack(),
  packNoStyle: makePack({ styleNotes: [], conditionalGaps: [], designGap: { vector: null, method: "pos-minus-neg", wants: [], complaints: [], reason: "x" } }),
  tokens: ["", "abc", "日本語", "mixed 日本 text", "😀", "a".repeat(10)].map((t) => ({ t, n: estimateTokens(t) })),
  dims: ["emo.valence", "asp.replayability", "meta.volume_log"].map((d) => ({ d, l: dimLabel(d) })),
  order: [[], ["周回"], ["Game"], ["難易度", "周回"]].map((terms) => ({ terms, ids: orderEvidence(parsedPack.pack.evidence, terms).map((e) => e.id) })),
  renders: budgets.flatMap((budgetTokens) => [["周回"], []].map((focusTerms) => ({
    budgetTokens, focusTerms, bundle: renderProxyBundle(parsedPack.pack, { budgetTokens, focusTerms, label: "論者#aaaaaa" }),
  }))),
  noStyle: renderProxyBundle(packNoStyle.pack, { budgetTokens: 5000, label: "論者#bbbbbb" }),
  low: renderProxyBundle({ ...parsedPack.pack, sufficiency: { ...parsedPack.pack.sufficiency, confidence: "low" } }, { budgetTokens: 5000, label: "x" }),
};

// ── proxy request / stance ─────────────────────────────────────────
const requests = [
  "代理: 論者#aaaaaa, 論者#bbbbbb 周回報酬は必要か",
  "周回 代理：論者#aaaaaa vs 論者#bbbbbb ジャンル: roguelike",
  "代理:論者#aaaaaa と 論者#aaaaaa、論者#cccccc&論者#dddddd / 議題",
  "ふつうの議題",
  "代理: なし 代理 : 論者#123abc",
  "代理: 論者#ABCDEF 大文字は不可",
  "代理: 論者#aaaaaa",
  "ジャンル: rpg_x-1 代理: 論者#aaaaaa,論者#bbbbbb",
  "代理: 論者#aaaaaa 論者#bbbbbbx 続き",
  "  / 代理: 論者#aaaaaa, 論者#bbbbbb ・、 ",
  "代理: 論者#aaaaaa, 論者#bbbbbb ジャンル: Bad",
];
out.requests = requests.map((text) => ({ text, request: parseProxyRequest(text) }));
const stanceTexts = ["con", "答え: neutral", "わからない", "PRO", "professional", "neutral pro", "pro, con", "con_x pro", "", "→pro←", "pro-con"];
out.stances = stanceTexts.map((text) => ({ text, stance: parseProxyStance(text) }));
out.stancePrompt = buildProxyStancePrompt("周回報酬は必要か", "束の本文");
out.sides = [
  [["con", "pro"], ["A", "B"]],
  [["pro", "pro"], ["A", "B"]],
  [["neutral", "con"], ["A"]],
  [["neutral", "neutral"], []],
].map(([stances, labels]) => ({ stances, labels, sides: assignIssueSides(stances as never, labels as string[]) }));

// ── design gap ─────────────────────────────────────────────────────
const mechanicsCases = [
  [],
  [{ name: "m", description: "", intended_valence: "positive", intended_aspects: ["fun", "story", "nope"], intended_emotions: ["joy"] }],
  [{ name: "a", description: "", intended_valence: "negative" }, { name: "b", description: "", intended_valence: "mixed" }, { name: "c", description: "" }],
];
out.designGap = {
  baseline: negativeBaselineVector(),
  targets: mechanicsCases.map((mechanics) => ({ mechanics, target: buildTargetVector(mechanics as never), gap: computeDesignGap(negativeBaselineVector(), buildTargetVector(mechanics as never)) })),
};

// ── notion ─────────────────────────────────────────────────────────
const depthTexts = ["深さ:3 で読んで", "depth=1", "深さ：99", "指示なし", "DEPTH : 4", "深さ:x depth: 2", "深さ:007", "depth:", "深さ 　：　2"];
out.notionDepth = depthTexts.map((text) => ({ text, depth: parseNotionDepth(text) ?? null }));
const urlTexts = [
  "仕様: https://www.notion.so/ws/Spec-0123456789abcdef0123456789abcdef。",
  "https://acme.notion.site/Page-1,https://acme.notion.site/Page-1 と HTTPS://APP.NOTION.COM/p/x)",
  "(https://notion.so/a) <https://example.com/x> https://xnotion.so/a https://evil.com/notion.so",
  "http://www.notion.so/a」』） https://www.notion.so/b?p=1#h!?",
  "https:// http://notion.site",
  "https://notion.so/日本語ページ、続き",
];
out.notionUrls = urlTexts.map((text) => ({ text, urls: findNotionUrls(text) }));
out.isNotion = ["https://www.notion.so/x", "ftp://notion.so/x", "https://notion.so.evil.com/x", "https://NOTION.SITE/a", "not a url"].map((u) => ({ u, ok: isNotionUrl(u) }));
const ROOT = "https://www.notion.so/ws/Spec-0123456789abcdef0123456789abcdef";
const warns: string[] = [];
out.notionExpand = {
  text: `${ROOT}。 深さ:1 https://acme.notion.site/Other`,
  md: await expandNotionLinks(`${ROOT}。 深さ:1 https://acme.notion.site/Other`, {
    deps: {
      api: null,
      crawlPublic: async (url, opts) => {
        if (url.includes("Other")) throw new Error("timeout");
        return {
          pages: [
            { url, title: "公開ページ", depth: 0, markdown: "公開本文\n" },
            { url: "", title: "", depth: 1, markdown: "子本文" },
            { url: "https://acme.notion.site/Deep", title: "深い", depth: 7, markdown: "" },
          ],
          errors: [{ url: "https://acme.notion.site/Gone", message: "404" }],
          truncated: opts.maxDepth === 1,
        };
      },
    },
    warn: (m) => warns.push(m),
  }),
  warns,
};

// ── voice preparation notice ───────────────────────────────────────
const states: VoicePreparation[] = [
  { referenceCount: 3, priorKnowledge: false, requestId: "di:s", requestStatus: "not_needed", choice: "unselected" },
  { referenceCount: 0, priorKnowledge: true, requestId: "di:s", requestStatus: "not_needed", choice: "unselected" },
  { referenceCount: 0, priorKnowledge: false, requestId: "di:s", requestStatus: "accepted", choice: "wait" },
  { referenceCount: 0, priorKnowledge: false, requestId: "di:s", requestStatus: "submitted", choice: "wait" },
  { referenceCount: 0, priorKnowledge: false, requestId: "di:s", requestStatus: "unavailable", choice: "unselected" },
  { referenceCount: 0, priorKnowledge: false, requestId: "di:s", requestStatus: "unavailable", choice: "unselected", failure: "down" },
  { referenceCount: 0, priorKnowledge: false, requestId: "di:s", requestStatus: "unknown", choice: "unselected", failure: "protocol" },
  { referenceCount: 0, priorKnowledge: false, requestId: "di:s", requestStatus: "submitted", choice: "unselected", failure: "auth" },
  { referenceCount: 0, priorKnowledge: false, requestId: "di:s", requestStatus: "unknown", choice: "unselected", failure: "configuration" },
];
out.voiceNotices = states.map((state) => ({ state, notice: voicePreparationNotice(state) }));

// ── anatomia refine ────────────────────────────────────────────────
const CARDS = [
  { name: "Trial & Judgment", description: "裁判", mechanics: ["trial", "death-or-education"], specRefs: [] },
  { name: "Client UI (PixiJS)", description: "描画", mechanics: [], specRefs: [] },
  { name: "Incident Lifecycle", description: "事件", mechanics: ["incident"], specRefs: [], rationale: "境界" },
];
const prompts: string[] = [];
const responses = [
  JSON.stringify([{ name: " 裁判 ", description: " 村人を糾弾 ", intended_affect: " 緊張 " }, { name: "", description: "x" }, [1], "s", { name: "事件", intended_affect: "" }, { name: "x", description: 3 }]),
  "前置き ```json\n[{\"name\":\"a\"}]\n``` 後書き ] ",
  "解析できませんでした",
  "[]",
  "[{\"name\": \"a\"} ] trailing [ ]",
];
const refined = [];
for (const r of responses) {
  const llm = new MockLLMClient([() => r]);
  const w: string[] = [];
  refined.push({ response: r, out: await refineDomainsToMechanics(CARDS as never, { llm, theme: "Pagus の面白さ", warn: (m) => w.push(m) }), warns: w });
  prompts.push(llm.lastInvocation?.prompt ?? "");
}
out.anatomia = { cards: CARDS, fallback: fallbackDomainsToMechanics(CARDS as never), refined, prompt: prompts[0] };
{
  const llm = new MockLLMClient([() => "[]"]);
  await refineDomainsToMechanics(CARDS as never, { llm });
  (out.anatomia as Record<string, unknown>).promptNoTheme = llm.lastInvocation?.prompt;
}

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "flow");
fs.mkdirSync(dir, { recursive: true });
fs.writeFileSync(path.join(dir, "w2h.json"), JSON.stringify(out, null, 2) + "\n");
console.log("wrote", path.join(dir, "w2h.json"));
