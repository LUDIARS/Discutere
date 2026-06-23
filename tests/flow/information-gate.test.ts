import assert from "node:assert/strict";

import {
  evaluateInformationDensity,
  runInformationGate,
  isDensity,
  type InformationGateConfig,
} from "../../src/flow/information-gate.js";
import type { ContextVoice } from "../../src/flow/discussion-paper.js";
import type { LLMClient } from "../../src/persona-engine/llm/client.js";

const dummyCore = {} as Parameters<typeof runInformationGate>[0]["core"];

/** density + gaps を JSON 文字列にする (LLM 応答の模擬)。 */
function evalJson(density: string, gaps: Array<Record<string, unknown>> = []): string {
  return JSON.stringify({ density, covered: [], gaps });
}

/** 与えた応答テキストを順に返す LLM (末尾は繰り返す)。 */
function llmReturning(texts: string[]): { client: LLMClient; calls: () => number } {
  let i = 0;
  const client: LLMClient = {
    async invoke() {
      const text = texts[Math.min(i, texts.length - 1)];
      i++;
      return { ok: true, text };
    },
  };
  return { client, calls: () => i };
}

/** 常に失敗する LLM。 */
const failingLlm: LLMClient = {
  async invoke() {
    return { ok: false, error: "down" };
  },
};

/** クロールで件数が増える listExternalVoices を作る。 */
function growingVoices() {
  let n = 0;
  return {
    list: (_terms: string[], _limit: number): ContextVoice[] =>
      Array.from({ length: n }, () => ({ content: "c", source: "niconico" }) as ContextVoice),
    add: (k: number) => {
      n += k;
    },
  };
}

/** 1 回のクロールで perCrawl 件取り込む fake (実 DB に触れない)。 query を記録する。 */
function fakeCollect(voices: ReturnType<typeof growingVoices>, perCrawl = 5) {
  const queries: string[] = [];
  const fn = (async (deps: { spec: { query?: string } }) => {
    queries.push(deps.spec.query ?? "");
    voices.add(perCrawl);
    return { collected: perCrawl, imported: perCrawl };
  }) as Parameters<typeof runInformationGate>[0]["collectAndImportFn"];
  return { fn, queries };
}

function baseConfig(over: Partial<InformationGateConfig> = {}): InformationGateConfig {
  return { enabled: true, minDensity: "moderate", maxLearnIterations: 2, maxGapsPerIteration: 2, ...over };
}

// --- isDensity ガード ---
assert.equal(isDensity("rich"), true);
assert.equal(isDensity("MODERATE"), false);
assert.equal(isDensity(3), false);

// --- evaluate: 正常 JSON を解釈 ---
{
  const { client } = llmReturning([evalJson("rich", [])]);
  const ev = await evaluateInformationDensity({ theme: "t", tags: [], voices: [], llm: client });
  assert.equal(ev.density, "rich");
  assert.deepEqual(ev.gaps, []);
}

// --- evaluate: LLM 失敗 → 件数ベース fallback (4 件 → moderate) ---
{
  const voices = Array.from({ length: 4 }, () => ({ content: "c", source: "s" }) as ContextVoice);
  const ev = await evaluateInformationDensity({ theme: "t", tags: [], voices, llm: failingLlm });
  assert.equal(ev.density, "moderate");
}

// --- evaluate: 非 JSON → fallback (0 件 → sparse) ---
{
  const client: LLMClient = { async invoke() { return { ok: true, text: "JSON ではない文章" }; } };
  const ev = await evaluateInformationDensity({ theme: "t", tags: [], voices: [], llm: client });
  assert.equal(ev.density, "sparse");
  assert.deepEqual(ev.gaps, []);
}

// --- evaluate: gaps 正規化 (query 欠落はテーマ+観点で補完, 不正 source は捨てる) ---
{
  const { client } = llmReturning([
    evalJson("sparse", [
      { aspect: "否定的意見", reason: "偏り", query: "", source: "reddit" },
      { aspect: "" }, // aspect 空 → 捨てる
    ]),
  ]);
  const ev = await evaluateInformationDensity({ theme: "周回設計", tags: [], voices: [], llm: client });
  assert.equal(ev.gaps.length, 1);
  assert.equal(ev.gaps[0].aspect, "否定的意見");
  assert.equal(ev.gaps[0].query, "周回設計 否定的意見"); // query 補完
  assert.equal(ev.gaps[0].source, undefined); // reddit は非対応 → undefined
}

// --- gate: sparse → 1 回学習 → rich で十分 ---
{
  const voices = growingVoices();
  const { fn, queries } = fakeCollect(voices);
  const { client } = llmReturning([
    evalJson("sparse", [{ aspect: "否定", reason: "r", query: "t 否定", source: "niconico" }]),
    evalJson("rich", []),
  ]);
  const r = await runInformationGate({
    core: dummyCore,
    theme: "t",
    tags: [],
    workspaceId: "knowledge",
    llm: client,
    listExternalVoices: voices.list,
    crawlSource: "niconico",
    config: baseConfig(),
    collectAndImportFn: fn,
  });
  assert.equal(r.ran, true);
  assert.equal(r.sufficient, true);
  assert.equal(r.initialDensity, "sparse");
  assert.equal(r.finalDensity, "rich");
  assert.equal(r.crawls, 1);
  assert.equal(r.importedTotal, 5);
  assert.deepEqual(queries, ["t 否定"]);
}

// --- gate: 常に sparse → 反復上限で打ち切り (sufficient=false) ---
{
  const voices = growingVoices();
  const { fn, queries } = fakeCollect(voices, 0); // 増えない
  const { client } = llmReturning([evalJson("sparse", [{ aspect: "a", reason: "r", query: "q1" }])]);
  const r = await runInformationGate({
    core: dummyCore,
    theme: "t",
    tags: [],
    workspaceId: "knowledge",
    llm: client,
    listExternalVoices: voices.list,
    crawlSource: "niconico",
    config: baseConfig({ maxLearnIterations: 2, maxGapsPerIteration: 1 }),
    collectAndImportFn: fn,
  });
  assert.equal(r.crawls, 2);
  assert.equal(r.sufficient, false);
  assert.equal(queries.length, 2); // 1 gap × 2 反復
}

// --- gate: 初回から moderate → 学習しない ---
{
  const voices = growingVoices();
  const { fn, queries } = fakeCollect(voices);
  const { client } = llmReturning([evalJson("moderate", [])]);
  const r = await runInformationGate({
    core: dummyCore,
    theme: "t",
    tags: [],
    workspaceId: "knowledge",
    llm: client,
    listExternalVoices: voices.list,
    crawlSource: "niconico",
    config: baseConfig({ minDensity: "moderate" }),
    collectAndImportFn: fn,
  });
  assert.equal(r.crawls, 0);
  assert.equal(r.sufficient, true);
  assert.equal(queries.length, 0);
}

// --- gate: LLM 全失敗でも材料が厚ければ degrade して通す (10 件 → rich fallback) ---
{
  const voices = growingVoices();
  voices.add(10);
  const { fn, queries } = fakeCollect(voices);
  const r = await runInformationGate({
    core: dummyCore,
    theme: "t",
    tags: [],
    workspaceId: "knowledge",
    llm: failingLlm,
    listExternalVoices: voices.list,
    crawlSource: "niconico",
    config: baseConfig({ minDensity: "moderate" }),
    collectAndImportFn: fn,
  });
  assert.equal(r.finalDensity, "rich");
  assert.equal(r.sufficient, true);
  assert.equal(r.crawls, 0);
  assert.equal(queries.length, 0);
}

// --- gate: gaps 空でも sparse なら テーマで 1 回学習 (fallback クエリ) ---
{
  const voices = growingVoices();
  const { fn, queries } = fakeCollect(voices, 0);
  const { client } = llmReturning([evalJson("sparse", [])]);
  const r = await runInformationGate({
    core: dummyCore,
    theme: "テーマX",
    tags: [],
    workspaceId: "knowledge",
    llm: client,
    listExternalVoices: voices.list,
    crawlSource: "niconico",
    config: baseConfig({ maxLearnIterations: 1, maxGapsPerIteration: 2 }),
    collectAndImportFn: fn,
  });
  assert.equal(r.crawls, 1);
  assert.deepEqual(queries, ["テーマX"]); // gap 無し → テーマで学習
}

// --- gate: 複数ソース横断 — 1 つの不足観点を全ソースで集める ---
{
  const voices = growingVoices();
  const calls: Array<{ source: string; query: string }> = [];
  const fn = (async (deps: { spec: { source: string; query?: string } }) => {
    calls.push({ source: deps.spec.source, query: deps.spec.query ?? "" });
    voices.add(5);
    return { collected: 5, imported: 5 };
  }) as Parameters<typeof runInformationGate>[0]["collectAndImportFn"];
  const { client } = llmReturning([
    evalJson("sparse", [{ aspect: "否定", reason: "r", query: "t 否定" }]),
    evalJson("rich", []),
  ]);
  const r = await runInformationGate({
    core: dummyCore,
    theme: "t",
    tags: [],
    workspaceId: "knowledge",
    llm: client,
    listExternalVoices: voices.list,
    crawlSources: ["niconico", "youtube"],
    config: baseConfig(),
    collectAndImportFn: fn,
  });
  assert.equal(r.crawls, 1);
  // 1 観点 × 2 ソース = 2 回クロール
  assert.equal(calls.length, 2, "全ソースでクロールする");
  assert.deepEqual(calls.map((c) => c.source).sort(), ["niconico", "youtube"]);
  assert.ok(calls.every((c) => c.query === "t 否定"));
  assert.equal(r.importedTotal, 10, "2 ソース分の取込が積算される");
}

// --- gate: crawlSources 未指定なら単数 crawlSource にフォールバック (後方互換) ---
{
  const voices = growingVoices();
  const { fn, queries } = fakeCollect(voices);
  const { client } = llmReturning([
    evalJson("sparse", [{ aspect: "a", reason: "r", query: "q" }]),
    evalJson("rich", []),
  ]);
  const r = await runInformationGate({
    core: dummyCore,
    theme: "t",
    tags: [],
    workspaceId: "knowledge",
    llm: client,
    listExternalVoices: voices.list,
    crawlSource: "niconico", // crawlSources 無し
    config: baseConfig(),
    collectAndImportFn: fn,
  });
  assert.equal(r.crawls, 1);
  assert.deepEqual(queries, ["q"], "単数 crawlSource で従来どおり 1 回");
}

console.log("information-gate.test.ts: all passed");
