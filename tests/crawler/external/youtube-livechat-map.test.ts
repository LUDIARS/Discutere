import assert from "node:assert/strict";

import {
  mapLiveChatRenderer,
  parseLiveChatJsonl,
  type LiveChatTextRenderer,
} from "../../../src/crawler/sources/youtube-livechat.js";

// --- mapLiveChatRenderer ---

const renderer: LiveChatTextRenderer = {
  id: "ChwABC123",
  timestampUsec: "1700000000000000",
  authorExternalChannelId: "UCxxxxxxxxxxxxxxxxxxxxxx",
  authorName: { simpleText: "ゲームユーザー太郎" },
  message: { runs: [{ text: "ここで震えた！" }, { emoji: { shortcuts: [":heart:"] } }] },
};

const u = mapLiveChatRenderer(renderer, "dQw4w9WgXcQ", "journey", 12345);
assert.ok(u, "有効なレンダラーから ExternalUtterance を生成できる");
assert.equal(u.source, "youtube-livechat");
assert.equal(u.nativeId, "lc_ChwABC123", "nativeId は lc_ プレフィックス付き");
assert.equal(u.gameSlug, "journey");
assert.equal(u.threadKey, "dQw4w9WgXcQ", "threadKey は videoId");
assert.equal(u.content, "ここで震えた！:heart:");
assert.equal(u.authorId, "UCxxxxxxxxxxxxxxxxxxxxxx");
assert.equal(u.authorName, "ゲームユーザー太郎");
assert.equal(u.postedAt, 1700000000000, "timestampUsec → epoch ms");
assert.equal(u.videoOffsetMs, 12345, "動画内オフセットを構造化フィールドで保持する");
assert.equal(u.sourceUrl, "https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=12", "オフセット秒付き URL");
assert.equal(u.signal, undefined, "通常メッセージは signal なし");
console.log("ok mapLiveChatRenderer basic");

// 空本文 (スタンプのみ) は null
const stampOnly = mapLiveChatRenderer(
  { id: "x", authorExternalChannelId: "UCy", message: { runs: [] } },
  "vid",
  "g",
  0
);
assert.equal(stampOnly, null, "空本文は null");
console.log("ok mapLiveChatRenderer empty content");

// スパチャ (isPaid=true) は signal 付き
const paid = mapLiveChatRenderer(renderer, "vid", "g", 0, true);
assert.ok(paid);
assert.deepEqual(paid.signal, { upvotes: 0 }, "スパチャは signal.upvotes=0");
console.log("ok mapLiveChatRenderer paid");

// --- parseLiveChatJsonl ---

function makeEvent(id: string, channelId: string, text: string, offsetMs: number): string {
  return JSON.stringify({
    replayChatItemAction: {
      actions: [
        {
          addChatItemAction: {
            item: {
              liveChatTextMessageRenderer: {
                id,
                timestampUsec: "1700000000000000",
                authorExternalChannelId: channelId,
                authorName: { simpleText: "user" },
                message: { runs: [{ text }] },
              },
            },
          },
        },
      ],
      videoOffsetTimeMsec: String(offsetMs),
    },
  });
}

const jsonl = [
  makeEvent("id1", "UCaaa", "神ゲー", 60000),
  makeEvent("id2", "UCbbb", "泣いた", 120000),
  '{"irrelevant": true}',   // replayChatItemAction 無し → スキップ
  "not-json",               // parse エラー → スキップ
  "",                       // 空行 → スキップ
].join("\n");

const items = parseLiveChatJsonl(jsonl, "vidXYZ", "shadow-of-the-colossus");
assert.equal(items.length, 2, "有効なチャット 2 件のみ取得");
assert.equal(items[0].content, "神ゲー");
assert.equal(items[0].sourceUrl, "https://www.youtube.com/watch?v=vidXYZ&t=60");
assert.equal(items[0].videoOffsetMs, 60000);
assert.equal(items[1].content, "泣いた");
assert.equal(items[1].sourceUrl, "https://www.youtube.com/watch?v=vidXYZ&t=120");
assert.equal(items[1].videoOffsetMs, 120000);
console.log("ok parseLiveChatJsonl");

// 空 JSONL は空配列
assert.equal(parseLiveChatJsonl("", "v", "g").length, 0);
assert.equal(parseLiveChatJsonl("\n\n", "v", "g").length, 0);
console.log("ok parseLiveChatJsonl empty");
