/**
 * ペーパーの使い回し (spec/feature/flow/paper-reuse.md) テスト。
 * - 「ペーパー#12」等の指定の読み取り (全角数字・paper 表記・指定なし)
 * - 声の保存 (置き換え・空は既存を消さない・上限) と読み出し
 * - 保存した声の検索 (検索語一致が先・一致なしでも返す)
 * - 議論番号からメカニクス・ゲーム内容・声を引く / 存在しない番号は null
 * - ゲーム内容の合成と知らせ文
 */

import assert from "node:assert/strict";
import path from "node:path";
import fs from "node:fs";

const TMP_DIR = path.resolve(".tmp/flow-paper-reuse-test");
fs.rmSync(TMP_DIR, { recursive: true, force: true });
fs.mkdirSync(TMP_DIR, { recursive: true });
process.env.DATABASE_PATH = path.join(TMP_DIR, "test.db");

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
const { _resetConfig } = await import("../../src/config.js");
_resetFlowDb();
_resetConfig();

const { parseReusePaperNo, loadReusablePaper, mergeReusedContext, describeReuse } = await import(
  "../../src/flow/paper-reuse.js"
);
const { savePaperVoices, loadPaperVoices, storedVoiceLookup, sessionVoiceLookup, PAPER_VOICE_LIMIT } = await import(
  "../../src/flow/paper-voices.js"
);
const { persistDraftPaper, getDiscussionNumberBySession } = await import("../../src/flow/discussion-paper.js");
const { paperDraftToMarkdown } = await import("../../src/flow/paper-markdown.js");

// ── 指定の読み取り ────────────────────────────────────────────────
{
  assert.equal(parseReusePaperNo("ペーパー#12 のメカニクスで議論"), 12);
  assert.equal(parseReusePaperNo("ペーパー ＃３"), 3);
  assert.equal(parseReusePaperNo("paper#7"), 7);
  assert.equal(parseReusePaperNo("ペーパー１２を使う"), 12);
  assert.equal(parseReusePaperNo("ガチャの天井を下げるか"), null);
  assert.equal(parseReusePaperNo("ペーパー#0"), null);
  assert.equal(parseReusePaperNo(undefined), null);
  console.log("  [ok] parseReusePaperNo");
}

// ── 声の保存と検索 ────────────────────────────────────────────────
{
  const voices = [
    { content: "天井が高すぎてつらい", source: "steam", sourceUrl: "https://example.com/1" },
    { content: "周回が楽しい", source: "youtube" },
    { content: "ガチャの天井は下げてほしい", source: "niconico" },
  ];
  assert.equal(savePaperVoices("s-voices", voices), 3);
  assert.deepEqual(loadPaperVoices("s-voices"), voices, "保存順・出所・URL を保つ");
  assert.equal(savePaperVoices("s-voices", []), 0);
  assert.equal(loadPaperVoices("s-voices").length, 3, "空の保存は既存を消さない");
  savePaperVoices("s-voices", voices.slice(0, 1));
  assert.equal(loadPaperVoices("s-voices").length, 1, "保存は置き換え");

  const many = Array.from({ length: PAPER_VOICE_LIMIT + 5 }, (_, i) => ({ content: `声 ${i}`, source: "steam" }));
  assert.equal(savePaperVoices("s-many", many), PAPER_VOICE_LIMIT, "上限で切る");

  const lookup = storedVoiceLookup(voices);
  assert.equal(lookup(["ガチャの天井"], 3)[0].content, "ガチャの天井は下げてほしい", "検索語を多く含む声が先");
  assert.equal(lookup(["まったく関係ない語"], 2).length, 2, "一致しなくても保存した声を返す");
  assert.equal(lookup(["天井"], 1).length, 1, "件数上限");

  assert.ok(sessionVoiceLookup("s-voices"), "保存があれば検索を返す");
  const fallback = () => [];
  assert.equal(sessionVoiceLookup("s-none", fallback), fallback, "保存が無ければ fallback");
  console.log("  [ok] 声の保存・検索");
}

// ── 議論番号から引く ──────────────────────────────────────────────
{
  const mechanics = [{ name: "ガチャ天井", description: "200 連で確定", source: "data" }];
  const bodyMd = paperDraftToMarkdown({
    theme: "天井を下げるか",
    tags: [],
    supplement: "",
    mechanics: mechanics as never,
    gameTitle: "テストゲーム",
    discussionTheme: "天井を下げるか",
    mechanicsContext: "ゲーム内容: 育成と周回のゲーム",
  });
  persistDraftPaper(
    { sessionId: "s-src", theme: "天井を下げるか", tags: [], mechanics: mechanics as never, supplement: "", bodyMd },
    "discussion"
  );
  savePaperVoices("s-src", [{ content: "天井が高すぎてつらい", source: "steam" }]);
  const no = getDiscussionNumberBySession("s-src");
  assert.ok(no, "議論番号が振られる");

  const reused = loadReusablePaper(no!);
  assert.ok(reused);
  assert.equal(reused!.sessionId, "s-src");
  assert.equal(reused!.mechanics[0].name, "ガチャ天井");
  assert.match(reused!.mechanicsContext, /育成と周回のゲーム/);
  assert.equal(reused!.voices.length, 1);
  assert.equal(loadReusablePaper(99999), null, "存在しない番号は null");

  assert.equal(mergeReusedContext(reused, "Notion 資料"), `${reused!.mechanicsContext}\n\nNotion 資料`);
  assert.equal(mergeReusedContext(reused, reused!.mechanicsContext), reused!.mechanicsContext, "同じ内容は重ねない");
  assert.equal(mergeReusedContext(null, undefined), undefined);

  assert.match(describeReuse(no!, reused), /メカニクス 1 件.*声 1 件 \(集め直しません\)/);
  assert.match(describeReuse(99999, null), /見つからない/);
  assert.match(describeReuse(no!, { ...reused!, voices: [] }), /保存が無いため通常どおり集めます/);
  console.log("  [ok] loadReusablePaper / mergeReusedContext / describeReuse");
}

_resetFlowDb();
delete process.env.DATABASE_PATH;
_resetConfig();
console.log("paper-reuse tests: all passed");
