import assert from "node:assert/strict";

import { fetchVideoComments } from "../../../src/crawler/sources/youtube-comments.js";
import { createQuotaTracker } from "../../../src/crawler/sources/youtube-quota.js";

function jsonResponse(body: unknown): Response {
  return { ok: true, status: 200, json: async () => body, text: async () => JSON.stringify(body) } as unknown as Response;
}
function errorResponse(status: number, body: string): Response {
  return { ok: false, status, json: async () => ({}), text: async () => body } as unknown as Response;
}

function thread(id: string, replies: string[] = []) {
  return {
    snippet: {
      topLevelComment: { id, snippet: { textOriginal: `c${id}`, authorChannelId: { value: `UC${id}` }, publishedAt: "2024-01-01T00:00:00Z", likeCount: 1 } },
      totalReplyCount: replies.length,
    },
    replies: { comments: replies.map((r) => ({ id: r, snippet: { textOriginal: `r${r}`, authorChannelId: { value: `UC${r}` }, publishedAt: "2024-01-01T00:00:00Z" } })) },
  };
}

// --- 1. 2 ページのページング + quota 加算 ---
let calls = 0;
const fakeFetch = (async (url: string) => {
  calls += 1;
  if (calls === 1) return jsonResponse({ items: [thread("a", ["a1"]), thread("b")], nextPageToken: "PAGE2" });
  assert.ok(String(url).includes("pageToken=PAGE2"), "2 ページ目は pageToken を載せる");
  return jsonResponse({ items: [thread("c")] }); // nextPageToken なし → 停止
}) as unknown as typeof fetch;

const quota = createQuotaTracker();
const items = await fetchVideoComments({ videoId: "VID", gameSlug: "vs", apiKey: "k", quota, fetchImpl: fakeFetch });
assert.equal(calls, 2, "2 ページで停止");
assert.equal(items.length, 4, "a + a1(返信) + b + c");
assert.equal(quota.spent(), 2, "commentThreads.list 2 回 = 2 units");
assert.equal(items[1].parentNativeId, "a", "返信 a1 は a を指す");
console.log("ok youtube fetch pagination");

// --- 2. コメント無効動画は空配列で skip (throw しない) ---
const disabledFetch = (async () => errorResponse(403, JSON.stringify({ error: { errors: [{ reason: "commentsDisabled" }] } }))) as unknown as typeof fetch;
const empty = await fetchVideoComments({ videoId: "X", gameSlug: "g", apiKey: "k", fetchImpl: disabledFetch });
assert.deepEqual(empty, [], "コメント無効 → 空配列");
console.log("ok youtube comments disabled skip");

// --- 3. maxComments で打ち切り ---
const manyFetch = (async () => jsonResponse({ items: [thread("1"), thread("2"), thread("3")] })) as unknown as typeof fetch;
const capped = await fetchVideoComments({ videoId: "V", gameSlug: "g", apiKey: "k", maxComments: 2, fetchImpl: manyFetch });
assert.equal(capped.length, 2, "maxComments=2 で打ち切り");
console.log("ok youtube maxComments cap");
