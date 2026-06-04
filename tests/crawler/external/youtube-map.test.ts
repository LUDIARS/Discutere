import assert from "node:assert/strict";

import {
  mapSearchVideo,
  mapPlaylistVideo,
} from "../../../src/crawler/sources/youtube-videos.js";
import {
  mapYoutubeComment,
  mapCommentThread,
  type CommentThreadRaw,
} from "../../../src/crawler/sources/youtube-comments.js";
import { createQuotaTracker, YT_COST } from "../../../src/crawler/sources/youtube-quota.js";

// --- video discovery mappers ---
const v = mapSearchVideo({
  id: { videoId: "abc123" },
  snippet: { title: "VS 考察", channelId: "UCchan", publishedAt: "2024-01-02T03:04:05Z" },
});
assert.ok(v);
assert.equal(v.videoId, "abc123");
assert.equal(v.channelId, "UCchan");
assert.equal(v.publishedAt, Date.parse("2024-01-02T03:04:05Z"));
assert.equal(mapSearchVideo({ id: {} }), null, "videoId 無し (channel結果) は除外");

const pv = mapPlaylistVideo({
  snippet: { title: "up", videoOwnerChannelId: "UCowner", resourceId: { videoId: "vid9" } },
  contentDetails: { videoId: "vid9", videoPublishedAt: "2023-05-06T00:00:00Z" },
});
assert.ok(pv);
assert.equal(pv.videoId, "vid9");
console.log("ok youtube video mappers");

// --- comment thread mapper (top + replies, responds_to) ---
const thread: CommentThreadRaw = {
  snippet: {
    topLevelComment: {
      id: "Ugtop",
      snippet: { textOriginal: "神ゲー", authorDisplayName: "taro", authorChannelId: { value: "UCtaro" }, publishedAt: "2024-02-01T00:00:00Z", likeCount: 12 },
    },
    totalReplyCount: 1,
  },
  replies: {
    comments: [
      { id: "Ugreply", snippet: { textOriginal: "わかる", authorDisplayName: "hana", authorChannelId: { value: "UChana" }, publishedAt: "2024-02-02T00:00:00Z", likeCount: 3 } },
    ],
  },
};

const us = mapCommentThread(thread, "VID", "vs");
assert.equal(us.length, 2);
assert.equal(us[0].nativeId, "Ugtop");
assert.equal(us[0].authorId, "UCtaro"); // channelId アンカー
assert.equal(us[0].authorName, "taro");
assert.equal(us[0].threadKey, "VID");
assert.equal(us[0].parentNativeId, undefined);
assert.equal(us[0].signal?.upvotes, 12);
assert.equal(us[1].nativeId, "Ugreply");
assert.equal(us[1].parentNativeId, "Ugtop", "返信は親コメントを指す");

const anon = mapYoutubeComment({ id: "x", snippet: { textOriginal: "t" } }, "V", "g");
assert.equal(anon.authorId, "unknown");
console.log("ok youtube comment mappers");

// --- quota ---
const q = createQuotaTracker(250);
assert.equal(q.remaining(), 250);
q.spend(YT_COST.search); // 100
assert.equal(q.spent(), 100);
assert.ok(q.canSpend(100));
q.spend(YT_COST.search); // 200
assert.equal(q.canSpend(100), false, "残 50 では search(100) 不可");
assert.throws(() => q.spend(YT_COST.search), /quota exceeded/);
q.spend(YT_COST.list); // 201, ok
assert.equal(q.spent(), 201);
console.log("ok youtube quota");
