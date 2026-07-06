import assert from "node:assert/strict";

import {
  ensureLearningData,
  countLearningVoices,
  isAutoCrawlSource,
  deriveSlug,
  type Collector,
} from "../../src/flow/learning-autocrawl.js";
import type { ContextVoice } from "../../src/flow/discussion-paper.js";
import type { ExternalUtterance } from "../../src/crawler/sources/types.js";

// listExternalVoices フェイク: 指定件数を返す。
function voicesOf(n: number) {
  return (_terms: string[], _limit: number): ContextVoice[] =>
    Array.from({ length: n }, () => ({}) as ContextVoice);
}

const dummyCore = {} as Parameters<typeof ensureLearningData>[0]["core"];

function fakeItems(n: number): ExternalUtterance[] {
  return Array.from(
    { length: n },
    (_v, i) =>
      ({
        source: "niconico",
        nativeId: `n${i}`,
        gameSlug: "x",
        threadKey: "t",
        content: "c",
        postedAt: 1,
        authorId: "anon",
        sourceUrl: "u",
      }) as ExternalUtterance
  );
}

// --- ヘルパ ---
assert.equal(isAutoCrawlSource("niconico"), true);
assert.equal(isAutoCrawlSource("reddit"), true);
assert.equal(deriveSlug("Hello World"), "hello-world");
assert.equal(deriveSlug("ローグライト"), "ローグライト"); // 日本語フォールバック
assert.equal(countLearningVoices("t", voicesOf(4), 3), 4);
assert.equal(countLearningVoices("t", undefined, 3), 0);

// --- 充足 → skip ---
{
  let collectorCalled = false;
  const collector: Collector = async () => {
    collectorCalled = true;
    return [];
  };
  const r = await ensureLearningData({
    core: dummyCore,
    theme: "ローグライト",
    slug: "rogue",
    workspaceId: "knowledge",
    spec: { source: "niconico" },
    minVoices: 3,
    listExternalVoices: voicesOf(5),
    collectors: { niconico: collector },
    importItems: () => 0,
  });
  assert.equal(r.skipped, true);
  assert.equal(r.crawled, false);
  assert.equal(r.voicesBefore, 5);
  assert.equal(collectorCalled, false, "充足時は collector を呼ばない");
}

// --- 不足 → クロール → 取込 ---
{
  let sawQuery: string | undefined;
  let imported = 0;
  const collector: Collector = async (ctx) => {
    sawQuery = ctx.spec.query?.trim() || ctx.theme; // 既定はテーマ
    return fakeItems(7);
  };
  const r = await ensureLearningData({
    core: dummyCore,
    theme: "周回設計",
    slug: "shuukai",
    workspaceId: "knowledge",
    spec: { source: "niconico" }, // query 省略 → テーマ
    minVoices: 3,
    listExternalVoices: voicesOf(1),
    collectors: { niconico: collector },
    importItems: (_c, items) => {
      imported = items.length;
      return items.length;
    },
  });
  assert.equal(r.skipped, false);
  assert.equal(r.crawled, true);
  assert.equal(r.imported, 7);
  assert.equal(imported, 7);
  assert.equal(sawQuery, "周回設計");
}

// --- 取得 0 件 → crawled=false ---
{
  const r = await ensureLearningData({
    core: dummyCore,
    theme: "無人気テーマ",
    slug: "x",
    workspaceId: "knowledge",
    spec: { source: "niconico" },
    minVoices: 3,
    listExternalVoices: voicesOf(0),
    collectors: { niconico: async () => [] },
    importItems: () => 0,
  });
  assert.equal(r.crawled, false);
  assert.equal(r.skipped, false);
  assert.equal(r.imported, 0);
}

// --- listExternalVoices 未指定 → 常に不足扱いでクロール ---
{
  const r = await ensureLearningData({
    core: dummyCore,
    theme: "t",
    slug: "x",
    workspaceId: "knowledge",
    spec: { source: "niconico" },
    minVoices: 3,
    collectors: { niconico: async () => fakeItems(2) },
    importItems: (_c, items) => items.length,
  });
  assert.equal(r.voicesBefore, 0);
  assert.equal(r.crawled, true);
  assert.equal(r.imported, 2);
}

console.log("learning-autocrawl.test.ts: all passed");
