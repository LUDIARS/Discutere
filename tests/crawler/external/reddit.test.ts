import assert from "node:assert/strict";

import {
  mapRedditComment,
  flattenComments,
  type RedditCommentRaw,
} from "../../../src/crawler/sources/reddit.js";

const ctx = { submissionId: "abc123", gameSlug: "shadow-of-the-colossus", permalink: "/r/gaming/comments/abc123/title/" };

// ── mapRedditComment ──
const top: RedditCommentRaw = {
  name: "t1_c1",
  id: "c1",
  body: "巨像戦は孤独で詩的だ。",
  author: "wanderer",
  created_utc: 1_700_000_000,
  parent_id: "t3_abc123", // スレッド直下
  ups: 42,
};
const u = mapRedditComment(top, ctx)!;
assert.equal(u.source, "reddit");
assert.equal(u.nativeId, "t1_c1");
assert.equal(u.threadKey, "abc123");
assert.equal(u.authorId, "wanderer"); // username = 公開アンカー
assert.equal(u.postedAt, 1_700_000_000 * 1000);
assert.equal(u.parentNativeId, undefined); // t3_ 親 → responds_to 無し
assert.equal(u.signal?.upvotes, 42);
assert.ok(u.content.includes("孤独"));

// 返信は parentNativeId を持つ
const reply = mapRedditComment(
  { name: "t1_c2", id: "c2", body: "同感", author: "agro", created_utc: 1, parent_id: "t1_c1", ups: 3 },
  ctx
)!;
assert.equal(reply.parentNativeId, "t1_c1");

// 削除済 / 空は null
assert.equal(mapRedditComment({ name: "t1_x", body: "[deleted]", created_utc: 1 }, ctx), null);
assert.equal(mapRedditComment({ name: "t1_y", body: "  ", created_utc: 1 }, ctx), null);
// author 欠落でも anonymous で取り込む
assert.equal(mapRedditComment({ name: "t1_z", body: "良い", created_utc: 1 }, ctx)!.authorId, "anonymous");
console.log("ok reddit map");

// ── flattenComments: ツリー平坦化 + 上限 ──
const tree = [
  {
    kind: "t1",
    data: {
      name: "t1_a",
      id: "a",
      body: "親コメント",
      author: "u1",
      created_utc: 1,
      parent_id: "t3_abc123",
      replies: {
        data: {
          children: [
            { kind: "t1", data: { name: "t1_b", id: "b", body: "子コメント", author: "u2", created_utc: 2, parent_id: "t1_a" } },
            { kind: "more", data: { count: 5 } }, // t1 でない → skip
          ],
        },
      },
    },
  },
  { kind: "t1", data: { name: "t1_c", id: "c", body: "[removed]", created_utc: 3, parent_id: "t3_abc123" } }, // drop
];
const flat = flattenComments(tree as never, ctx, 100);
assert.equal(flat.length, 2, "親 + 子 (removed と more は除外)");
assert.deepEqual(flat.map((f) => f.nativeId).sort(), ["t1_a", "t1_b"]);
// 上限
assert.equal(flattenComments(tree as never, ctx, 1).length, 1, "maxComments で打ち切り");
console.log("ok reddit flatten");

console.log("reddit.test.ts: all passed");
