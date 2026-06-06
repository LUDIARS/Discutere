import assert from "node:assert/strict";

import {
  extractArticle,
  mapWebsiteArticle,
  normalizeUrl,
  fetchWebsiteArticles,
} from "../../../src/crawler/sources/website.js";

// ── normalizeUrl: hash / tracking パラメータ除去 ──
assert.equal(
  normalizeUrl("https://example.com/a?utm_source=x&id=5#frag"),
  "https://example.com/a?id=5"
);
assert.equal(normalizeUrl("https://example.com/p?fbclid=abc"), "https://example.com/p");
assert.equal(normalizeUrl("not a url"), "not a url");

// ── extractArticle: タグ除去・本文抽出・著者 ──
const html = `<!doctype html><html><head>
  <title>ワンダと巨像レビュー | Example</title>
  <meta name="author" content="山田 太郎">
  <meta property="og:title" content="ワンダと巨像レビュー">
  <style>.x{color:red}</style>
</head><body>
  <nav>メニュー <a href="/">home</a></nav>
  <article>
    <h1>ワンダと巨像レビュー</h1>
    <p>巨像との戦いは孤独で詩的だ。</p>
    <p>広大なフィールドに敵は巨像だけ、という大胆な設計が&quot;余白&quot;を生む。</p>
    <script>tracker();</script>
  </article>
  <footer>copyright</footer>
</body></html>`;

const art = extractArticle(html);
assert.equal(art.title, "ワンダと巨像レビュー");
assert.equal(art.author, "山田 太郎");
assert.ok(art.text.includes("孤独で詩的"), "本文を抽出");
assert.ok(art.text.includes('"余白"'), "エンティティ復号");
assert.ok(!art.text.includes("tracker"), "script を除去");
assert.ok(!art.text.includes("メニュー"), "nav を除去");
console.log("ok website extract");

// ── mapWebsiteArticle: ExternalUtterance 化 ──
const u = mapWebsiteArticle({
  url: "https://www.example.com/review?utm_medium=mail#c",
  gameSlug: "shadow-of-the-colossus",
  fetchedAt: 1_700_000_000_000,
  article: art,
});
assert.equal(u.source, "website");
assert.equal(u.nativeId, "https://www.example.com/review"); // 正規化済み (utm/hash 除去、 host は維持)
assert.equal(u.threadKey, u.nativeId); // 記事 1 本 = 1 スレッド
assert.equal(u.gameSlug, "shadow-of-the-colossus");
assert.equal(u.authorId, "example.com:山田 太郎"); // アンカーは host から www を除去
assert.equal(u.lang, "ja");
assert.ok(u.content.startsWith("ワンダと巨像レビュー\n\n"), "タイトル + 本文");
console.log("ok website map");

// 著者が無ければ host をアンカーに
const noAuthor = mapWebsiteArticle({
  url: "https://blog.test/p",
  gameSlug: "g",
  fetchedAt: 1,
  article: { title: "T", text: "english review text body here long enough" },
});
assert.equal(noAuthor.authorId, "blog.test");
assert.equal(noAuthor.lang, "en");

// ── fetchWebsiteArticles: HTTP モック + 失敗 URL は skip ──
const fetchMock = (async (url: string) => {
  if (url.includes("dead")) return { ok: false, status: 404 } as Response;
  return {
    ok: true,
    status: 200,
    async text() {
      return `<html><body><article><p>${"とても面白いゲームだと思う。".repeat(10)}</p></article></body></html>`;
    },
  } as Response;
}) as unknown as typeof fetch;

const items = await fetchWebsiteArticles({
  urls: ["https://good.test/1", "https://dead.test/2"],
  gameSlug: "g",
  politeDelayMs: 0,
  now: () => 42,
  fetchImpl: fetchMock,
  onError: () => {},
});
assert.equal(items.length, 1, "生存 URL のみ取り込む (404 は skip)");
assert.equal(items[0].postedAt, 42);
assert.ok(items[0].content.includes("面白い"));
console.log("ok website fetch (mock + skip)");

console.log("website tests: all passed");
