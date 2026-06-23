/**
 * RAG retrieval (外部の声) のテスト — spec/feature/crawler/EXTERNAL-SOURCES.md §14。
 *
 * adapter.listRelevantExternalVoices が:
 *  - 議題語に一致する外部取り込み発話 (speaker_id ext:%) を拾う
 *  - 個人を 論者# へマスクし、 出所 (source/sourceUrl) を attribution から付与する (§6)
 *  - persona/Discord 内部発話と、 関連語も支持も無いノイズは返さない
 * を検証する。
 */
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { createDiscatierContextProvider } from "../../src/discatier-engine-adapter/index.js";
import { openAttributionStore } from "../../src/crawler/sources/attribution-store.js";

const workDir = path.resolve(".tmp/external-voices");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const core = createCore(path.join(workDir, "kg.kuzu"), path.join(workDir, "events.jsonl"));
const ws = "knowledge";
const OLD = Date.now() - 5_000;

// 関連語ヒットする外部発話 (YouTube)。
const hitId = core.repos.utterance.create({
  workspaceId: ws,
  sessionId: "ext:youtube:vid1",
  speakerId: "ext:youtube:UCabc",
  rawContent: "このゲームはボス戦が理不尽すぎると思う",
  postedAt: OLD,
});
// 関連語も支持スコアも無い外部発話 (拾われないはず)。
core.repos.utterance.create({
  workspaceId: ws,
  sessionId: "ext:youtube:vid1",
  speakerId: "ext:youtube:UCxyz",
  rawContent: "今日は良い天気ですね",
  postedAt: OLD,
});
// persona / 内部発話 (ext: でない → 対象外)。
core.repos.utterance.create({
  workspaceId: ws,
  sessionId: "discord:1/2",
  speakerId: "persona:advocate",
  rawContent: "ボス戦は歯ごたえがあって良い",
  postedAt: OLD,
});

// 出所メタを付与 (§6: 露出時に source/URL を出す素)。
const attribution = openAttributionStore();
attribution.set({
  utteranceId: hitId,
  source: "youtube",
  sourceUrl: "https://www.youtube.com/watch?v=vid1&lc=c1",
  nativeId: "c1",
  gameSlug: "demo-game",
});
attribution.close();

const adapter = createDiscatierContextProvider(core);
const voices = adapter.listRelevantExternalVoices!(ws, ["ボス戦"], 6);

assert.equal(voices.length, 1, "関連語に一致する外部発話だけ拾う (persona/ノイズは除外)");
const v = voices[0];
assert.equal(v.id, hitId, "拾うのはキーワード一致した外部発話");
assert.equal(v.source, "youtube", "attribution の source を開示");
assert.ok(v.sourceUrl?.includes("youtube.com"), "attribution の sourceUrl を開示");
assert.ok(/^論者#[0-9a-f]+$/.test(v.speakerLabel), "個人はペルソナ名へマスク (公開IDを出さない)");
assert.ok(!v.speakerLabel.includes("UCabc"), "公開 channelId が露出しない");
assert.ok(v.score > 0, "合成スコアが正");
console.log("ok external-voices retrieval (keyword + mask + source)");

// 関連語が空 + 支持スコア無し → 何も返さない (議論を薄めない)。
const none = adapter.listRelevantExternalVoices!(ws, [], 6);
assert.equal(none.length, 0, "関連語も支持も無ければ外部の声は注入しない");
console.log("ok external-voices empty when no relevance");

core.close();
console.log("external-voices tests: all passed");
