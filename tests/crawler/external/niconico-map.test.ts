import assert from "node:assert/strict";

import { mapNicoComment } from "../../../src/crawler/sources/niconico.js";

const utterance = mapNicoComment("sm9", "sample-game", {
  id: "comment-1",
  body: "888888",
  postedAt: "2026-07-12T00:00:00.000Z",
  userId: "masked-user",
  vposMs: 45678,
});

assert.equal(utterance.videoOffsetMs, 45678);
assert.equal(utterance.postedAt, Date.parse("2026-07-12T00:00:00.000Z"));
assert.equal(utterance.sourceUrl, "https://www.nicovideo.jp/watch/sm9");

console.log("ok mapNicoComment video offset");
