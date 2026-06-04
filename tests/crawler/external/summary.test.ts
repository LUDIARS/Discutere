import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../../src/core/index.js";
import { importExternalUtterances } from "../../../src/crawler/sources/importer.js";
import { openIngestedStore } from "../../../src/crawler/sources/ingested-store.js";
import { openRawStore } from "../../../src/crawler/sources/raw-store.js";
import { summarizeItems, type Summarizer } from "../../../src/crawler/sources/summarize.js";
import type { ExternalUtterance } from "../../../src/crawler/sources/types.js";

const workDir = path.resolve(".tmp/crawler-summary");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

// ── summarizeItems: 長文だけ要約、 短文はそのまま ──
const longText = "ワンダと巨像のレビュー本文。".repeat(60); // > 600 字
const items: ExternalUtterance[] = [
  { source: "website", nativeId: "u-long", gameSlug: "sotc", threadKey: "t1", content: longText, postedAt: 1, authorId: "famitsu.com", sourceUrl: "https://famitsu.com/a" },
  { source: "website", nativeId: "u-short", gameSlug: "sotc", threadKey: "t2", content: "短い感想", postedAt: 2, authorId: "blog.test", sourceUrl: "https://blog.test/b" },
];

const fakeSummarizer: Summarizer = {
  async summarize(text) {
    return `要約(${text.length}字の記事)`;
  },
};
const n = await summarizeItems(items, fakeSummarizer, { minChars: 600 });
assert.equal(n, 1, "長文 1 件だけ要約");
assert.ok(items[0].summary?.startsWith("要約("), "長文に summary 付与");
assert.equal(items[1].summary, undefined, "短文は要約しない");
console.log("ok summarizeItems");

// ── importer: summary→本文、 raw 全文は raw-store へ ──
const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
const ingested = openIngestedStore(path.join(workDir, "ingested.sqlite"));
const rawStore = openRawStore(path.join(workDir, "raw.sqlite"));

const r = importExternalUtterances(core, items, { workspaceId: "knowledge", ingested, rawStore });
assert.equal(r.utterances, 2);
assert.equal(r.summarized, 1, "2 層化したのは長文 1 件");

const long = core.client.raw.prepare("SELECT raw_content FROM utterances WHERE id = ?").get("ext:website:u-long") as { raw_content: string };
assert.ok(long.raw_content.startsWith("要約("), "本文は要約 (参照される側 = トークン節約)");
const short = core.client.raw.prepare("SELECT raw_content FROM utterances WHERE id = ?").get("ext:website:u-short") as { raw_content: string };
assert.equal(short.raw_content, "短い感想", "短文は raw のまま");

const raw = rawStore.get("ext:website:u-long");
assert.ok(raw, "raw-store に退避");
assert.equal(raw!.rawText, longText, "raw 全文を保持");
assert.equal(raw!.url, "https://famitsu.com/a");
assert.equal(rawStore.get("ext:website:u-short"), null, "短文は raw-store に入れない");
console.log("ok importer 2-layer (summary→本文 / raw→store)");

// ── rawStore なしなら raw を本文に (従来挙動) ──
const core2 = createCore(path.join(workDir, "db2.kuzu"), path.join(workDir, "events2.jsonl"));
const r2 = importExternalUtterances(core2, [{ ...items[0], nativeId: "u-long2" }], { workspaceId: "knowledge" });
assert.equal(r2.summarized, 0);
const body = core2.client.raw.prepare("SELECT raw_content FROM utterances WHERE id = ?").get("ext:website:u-long2") as { raw_content: string };
assert.equal(body.raw_content, longText, "rawStore 未指定なら raw を本文に");
console.log("ok no rawStore → raw fallback");

rawStore.close();
ingested.close();
core.close();
core2.close();
console.log("summary.test.ts: all passed");
