import assert from "node:assert/strict";

import {
  classifyCrawlUrl,
  extractUrls,
  formatCrawlSummary,
  parseCrawlMessage,
  slugFromTarget,
} from "../../src/discord-hook/crawl-channel.js";

// ── extractUrls ──
assert.deepEqual(extractUrls("見て https://a.test/x これと http://b.test/y。"), [
  "https://a.test/x",
  "http://b.test/y",
]);
assert.deepEqual(extractUrls("URL なし"), []);

// ── parseCrawlMessage: slug ヒント ──
assert.deepEqual(parseCrawlMessage("[sotc] https://a.test/x"), {
  gameSlug: "sotc",
  urls: ["https://a.test/x"],
});
assert.deepEqual(parseCrawlMessage("sotc: https://a.test/x https://a.test/y"), {
  gameSlug: "sotc",
  urls: ["https://a.test/x", "https://a.test/y"],
});
assert.deepEqual(parseCrawlMessage("https://a.test/x"), { gameSlug: undefined, urls: ["https://a.test/x"] });
console.log("ok crawl parse");

// ── classifyCrawlUrl ──
assert.deepEqual(classifyCrawlUrl("https://store.steampowered.com/app/1794680/Vampire_Survivors/"), {
  kind: "steam",
  url: "https://store.steampowered.com/app/1794680/Vampire_Survivors/",
  appId: 1794680,
});
assert.equal(classifyCrawlUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ").kind, "youtube");
assert.equal(classifyCrawlUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ").videoId, "dQw4w9WgXcQ");
assert.equal(classifyCrawlUrl("https://youtu.be/abc123XYZ_-").videoId, "abc123XYZ_-");
assert.equal(classifyCrawlUrl("https://famitsu.com/news/x.html").kind, "website");
// youtube のトップ/検索は videoId 無し → website 扱い
assert.equal(classifyCrawlUrl("https://www.youtube.com/results?search_query=x").kind, "website");
// reddit スレッド → submissionId
const rd = classifyCrawlUrl("https://www.reddit.com/r/gaming/comments/abc123/some_title/");
assert.equal(rd.kind, "reddit");
assert.equal(rd.submissionId, "abc123");
console.log("ok crawl classify");

// ── slugFromTarget ──
assert.equal(slugFromTarget({ kind: "steam", url: "x", appId: 42 }), "steam-42");
assert.equal(slugFromTarget({ kind: "youtube", url: "x", videoId: "vid" }), "yt-vid");
assert.equal(slugFromTarget({ kind: "reddit", url: "x", submissionId: "abc" }), "reddit-abc");
assert.equal(slugFromTarget({ kind: "website", url: "https://www.example.com/p" }), "example-com");
console.log("ok crawl slug");

// ── formatCrawlSummary ──
const sum = formatCrawlSummary([
  { url: "https://a.test", kind: "website", gameSlug: "g", fetched: 1, imported: 1, skipped: 0 },
  { url: "https://b.test", kind: "steam", gameSlug: "steam-1", fetched: 0, imported: 0, skipped: 0, error: "boom" },
]);
assert.ok(sum.includes("データ追加"));
assert.ok(sum.includes("計 1 件"));
assert.ok(sum.includes("✅"));
assert.ok(sum.includes("⚠️"));
assert.ok(sum.includes("boom"));
console.log("ok crawl summary");

console.log("crawl-channel.test.ts: all passed");
