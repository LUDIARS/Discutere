import assert from "node:assert/strict";

import {
  toSpeakerId,
  maskedPersonaLabel,
  sourceBadge,
  maskedSpeakerWithSource,
} from "../../../src/crawler/sources/persona.js";

// 保管層: 公開 ID をそのまま安定アンカー化
assert.equal(toSpeakerId("steam", "76561199058015945"), "ext:steam:76561199058015945");
assert.equal(toSpeakerId("youtube", "UCabc"), "ext:youtube:UCabc");

// 露出層: 決定的・不可逆・公開 ID を含まない
const sid = toSpeakerId("steam", "76561199058015945");
const label1 = maskedPersonaLabel(sid);
const label2 = maskedPersonaLabel(sid);
assert.equal(label1, label2, "同一 speaker_id は同じ表示名 (人物の一貫性)");
assert.ok(label1.startsWith("論者#"), "ペルソナ表示名フォーマット");
assert.ok(!label1.includes("76561199058015945"), "露出名に公開 ID を含まない (逆引き不可)");
assert.notEqual(maskedPersonaLabel(toSpeakerId("steam", "1")), label1, "別人は別表示名");

console.log("ok persona anchor + mask");

// ─── 二層シリアライズ (§6「出所は透明 / 個人は仮名」) の回帰テスト ───
// 個人 (speaker_id = 公開 ID アンカー) はマスク、出所 (source/url) は素通しで開示する。
{
  const authorId = "76561199058015945";
  const speakerId = toSpeakerId("steam", authorId);
  // 出所 URL は「出所は透明」方針で素通しする。ここでは個人 ID を含まないスレッド URL を使い、
  // 露出に ID が紛れ込むのは speaker マスク漏れ以外に無い状態でテストする。
  const attribution = { source: "steam", sourceUrl: "https://steamcommunity.com/app/123/discussions/0/thread/" };

  // sourceBadge: attribution があれば source + url を開示、無ければ null
  assert.deepEqual(sourceBadge(attribution), { source: "steam", url: attribution.sourceUrl }, "出所バッジは透明");
  assert.equal(sourceBadge(null), null, "attribution 無しは出所バッジ null");
  assert.equal(sourceBadge(undefined), null, "attribution undefined も null");

  // maskedSpeakerWithSource: 個人はマスク / 出所は透明
  const exposed = maskedSpeakerWithSource(speakerId, attribution);
  assert.ok(exposed.speaker.startsWith("論者#"), "話者は仮名");
  assert.equal(exposed.source, "steam", "出所 source は素通し");
  assert.equal(exposed.sourceUrl, attribution.sourceUrl, "出所 url は素通し");

  // 最重要: 露出オブジェクトのどこにも生の authorId / speakerId アンカーが漏れない
  // (出所 URL に個人 ID は含まれないので、漏れるとすれば speaker マスクの破れだけ)
  const serialized = JSON.stringify(exposed);
  assert.ok(!serialized.includes(authorId), "露出に生の authorId を含まない");
  assert.ok(!serialized.includes(speakerId), "露出に speaker_id アンカー (ext:..) を含まない");
  assert.ok(!exposed.speaker.includes(authorId), "話者ラベルに公開 ID を含まない");

  // 内部 Discord 発話など attribution 無し: 出所は null・話者は仮名のまま
  const internalOnly = maskedSpeakerWithSource(toSpeakerId("discord", "user-999"), null);
  assert.ok(internalOnly.speaker.startsWith("論者#"), "出所無しでも話者は仮名");
  assert.equal(internalOnly.source, null, "出所無しは source null");
  assert.equal(internalOnly.sourceUrl, null, "出所無しは url null");
  assert.ok(!internalOnly.speaker.includes("user-999"), "出所無しでも公開 ID を漏らさない");
}

console.log("ok persona two-layer serialize (出所透明 / 個人仮名)");
