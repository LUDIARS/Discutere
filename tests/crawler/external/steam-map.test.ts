import assert from "node:assert/strict";

import { mapSteamReview, type SteamReviewRaw } from "../../../src/crawler/sources/steam.js";

const raw: SteamReviewRaw = {
  recommendationid: "199734567",
  author: { steamid: "76561199058015945", num_games_owned: 42, playtime_forever: 610 },
  review: "中毒性が高い。レベルアップの選択が悩ましくて楽しい。",
  language: "japanese",
  timestamp_created: 1779333194,
  voted_up: true,
  votes_up: 3,
};

const u = mapSteamReview(1794680, "vampire-survivors", raw);
assert.equal(u.source, "steam");
assert.equal(u.nativeId, "199734567");
assert.equal(u.gameSlug, "vampire-survivors");
assert.equal(u.threadKey, "1794680");
assert.equal(u.authorId, "76561199058015945"); // SteamID64 を保持
assert.equal(u.postedAt, 1779333194 * 1000); // 秒 → ms
assert.equal(u.lang, "japanese");
assert.equal(u.signal?.votedUp, true);
assert.equal(u.signal?.upvotes, 3);
assert.equal(u.parentNativeId, undefined); // Steam は非スレッド
assert.ok(u.content.includes("中毒性"));
assert.ok(u.sourceUrl.includes("76561199058015945"));

// steamid 欠落でも落ちない
const anon = mapSteamReview(1, "x", { recommendationid: "1", review: "r", timestamp_created: 1, voted_up: false });
assert.equal(anon.authorId, "unknown");
assert.equal(anon.signal?.votedUp, false);

console.log("ok steam map");
