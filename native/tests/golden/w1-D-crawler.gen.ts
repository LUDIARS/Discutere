// crawler (md-format / sources の純関数) と slack・flow・design-diagnostic の決まった出力を記録する。
// 実行: npx tsx native/tests/golden/w1-D-crawler.gen.ts
// 出力: native/tests/fixtures/crawler/*.json, fixtures/slack/blocks.json, fixtures/flow/premise.json
import fs from "node:fs";
import path from "node:path";
import { parseGameKG } from "../../../src/crawler/md-format.js";
import { extractArticle, mapWebsiteArticle, normalizeUrl } from "../../../src/crawler/sources/website.js";
import { maskedPersonaLabel, maskedSpeakerWithSource, toSpeakerId } from "../../../src/crawler/sources/persona.js";
import { extractNvComment, mapNicoComment } from "../../../src/crawler/sources/niconico.js";
import { mapSteamReview } from "../../../src/crawler/sources/steam.js";
import { flattenComments, mapRedditComment } from "../../../src/crawler/sources/reddit.js";
import { mapCommentThread } from "../../../src/crawler/sources/youtube-comments.js";
import { mapPlaylistVideo, mapSearchVideo } from "../../../src/crawler/sources/youtube-videos.js";
import { buildFlowPickBlocks, buildFlowSettingsBlocks } from "../../../src/slack/slack-entry.js";
import { projectPremise, requiresProjectTitle } from "../../../src/flow/project-premise.js";
import { DIAGNOSTIC_PROMPT_CONTRACT } from "../../../src/design-diagnostic/prompt-contract.js";
import { mapImpression } from "../../../src/flow/user-voices/voluptas-impressions.js";

const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures");
const write = (sub: string, name: string, value: unknown) => {
  fs.mkdirSync(path.join(root, sub), { recursive: true });
  fs.writeFileSync(path.join(root, sub, name), JSON.stringify(value, null, 2) + "\n");
};
const run = <T>(fn: () => T) => {
  try {
    return { ok: true, value: JSON.parse(JSON.stringify(fn())) };
  } catch (e) {
    return { ok: false, error: (e as Error).message };
  }
};

const md = [
  "---\nid: my-game\ntitle: My Game\n---\n",
  "---\nid: my-game\ntitle: My Game\ngenre: action\nworkspace_id: ws\nsources:\n  - url: https://example.com\n    title: T\n    excerpt_policy: summary-only\nmechanics:\n  - name: ガチャ\n    description: 確率\n    intended_valence: positive\n    intended_aspects: [fun, '', 3]\n    intended_emotions: []\n  - name: 協力\n    intended_valence: odd\naesthetics:\n  - name: 美\n---\n\n# Body\n",
  "---\nid: Bad_ID\ntitle: x\n---\n",
  "---\ntitle: x\n---\n",
  "---\nid: 123\ntitle: x\n---\n",
  "---\nid: a\ntitle: '  '\n---\n",
  "---\nid: a\ntitle: t\nworkspace_id: '  '\n---\n   \n",
  "---\nid: a\ntitle: t\nsources:\n  - 3\n---\n",
  "---\nid: a\ntitle: t\nsources:\n  - [x]\n---\n",
  "---\nid: a\ntitle: t\nsources:\n  - url: u\n    excerpt_policy: bad\n---\n",
  "---\nid: a\ntitle: t\nsources:\n  - url: u\n    excerpt_policy:\n---\n",
  "---\nid: a\ntitle: t\nmechanics:\n  - description: d\n---\n",
  "---\nid: a\ntitle: t\naesthetics:\n  - name: ' '\n---\n",
  "---\nid: a\ntitle: t\nmechanics: notarray\n---\nbody",
  "no front matter",
];
write("crawler", "md_format.json", md.map((input) => ({ input, ...run(() => parseGameKG(input, "x.md")) })));

const html1 = `<!doctype html><html><head>
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
const htmls = [
  html1,
  "<html><body><main><p>main text &amp; more</p></main></body></html>",
  "<html><head><title> T &amp;amp; </title><meta property='article:author' content=' Ann '></head><body><!-- c --><p>x<br>y</p><form>f</form></body></html>",
  "<p>no body tag</p><h1>Heading</h1>",
  "<html><body><article></article><main>  </main><p>fallback</p></body></html>",
  "<META PROPERTY=\"og:title\" CONTENT=\"Upper\"><TITLE>t</TITLE><ARTICLE>A &#x3042; &#12354;</ARTICLE>",
  `<article>${"あ".repeat(9000)}</article>`,
];
write("crawler", "website.json", {
  extract: htmls.map((h) => ({ input: h, output: extractArticle(h) })),
  normalize: [
    "https://example.com/a?utm_source=x&id=5#frag",
    "https://example.com/p?fbclid=abc",
    "not a url",
    "  https://Example.COM:443/A?b=1#x ",
    "https://example.com/p?ref=1&a=b%20c&UTM_x=2",
    "https://example.com/?",
    "http://example.com:8080/記事?q=日本",
    "https://www.example.com/review?utm_medium=mail#c",
  ].map((u) => ({ input: u, output: normalizeUrl(u) })),
  map: [
    mapWebsiteArticle({ url: "https://www.example.com/review?utm_medium=mail#c", gameSlug: "g", fetchedAt: 1, article: extractArticle(html1) }),
    mapWebsiteArticle({ url: "https://blog.test/p", gameSlug: "g", fetchedAt: 1, article: { title: "", text: "english text" } }),
    mapWebsiteArticle({ url: "https://blog.test/p", gameSlug: "g", fetchedAt: 1, article: { title: "T", text: "x", author: "" } }),
  ],
});

write("crawler", "persona.json", {
  labels: ["ext:steam:1", "ext:youtube:UCabc", "", "ext:discord:user-999"].map((s) => ({ input: s, output: maskedPersonaLabel(s) })),
  speakerId: toSpeakerId("steam", "765"),
  masked: [
    maskedSpeakerWithSource("ext:steam:1", { source: "steam", sourceUrl: "https://s/" }),
    maskedSpeakerWithSource("ext:steam:1", null),
  ],
});

const sr = { data: { response: { comment: { nvComment: { threadKey: "tk", server: "https://nv.example", params: { targets: [{ id: "1", fork: "main" }], language: "ja-jp" } } } } } };
const enc = JSON.stringify(sr).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;");
write("crawler", "sources.json", {
  nv: [
    `<html><meta name="server-response" content="${enc}"></html>`,
    `<meta name="server-response" content=""><meta name="server-response" content="${enc}">`,
    `<meta name="server-response" content="{broken">`,
    `<meta name="server-response" content="${JSON.stringify({ data: {} }).replace(/"/g, "&quot;")}">`,
    "<html></html>",
  ].map((h) => ({ input: h, output: extractNvComment(h) })),
  nico: [
    mapNicoComment("sm9", "g", { id: "c1", body: "888", postedAt: "2026-07-12T00:00:00.000Z", userId: "u", vposMs: 45678 }),
    mapNicoComment("sm9", "g", { no: 12, body: "x" }),
    mapNicoComment("sm9", "g", { body: "x", postedAt: "" }),
  ],
  steam: [
    mapSteamReview(1794680, "vs", { recommendationid: "1", author: { steamid: "765" }, review: "r", language: "japanese", timestamp_created: 1779333194, voted_up: true, votes_up: 3 }),
    mapSteamReview(1, "x", { recommendationid: "2", review: "r", timestamp_created: 1, voted_up: false }),
  ],
  reddit: [
    mapRedditComment({ name: "t1_c1", id: "c1", body: "b", author: "w", created_utc: 1700000000, parent_id: "t3_a", ups: 42 }, { submissionId: "a", gameSlug: "g", permalink: "/r/x/comments/a/t/" }),
    mapRedditComment({ id: "t1_c2", body: "b", author: "[deleted]", parent_id: "t1_c1" }, { submissionId: "a", gameSlug: "g" }),
    mapRedditComment({ body: "b" }, { submissionId: "a", gameSlug: "g" }),
  ],
  redditFlatten: flattenComments(
    [
      { kind: "t1", data: { name: "t1_a", id: "a", body: "p", author: "u1", created_utc: 1, replies: { data: { children: [{ kind: "t1", data: { name: "t1_b", id: "b", body: "c", created_utc: 2, parent_id: "t1_a" } }, { kind: "more", data: {} }] } } } },
      { kind: "t1", data: { name: "t1_c", id: "c", body: "q", created_utc: 3, replies: "" } },
    ] as never,
    { submissionId: "s", gameSlug: "g" },
    10
  ),
  youtube: mapCommentThread(
    {
      snippet: { topLevelComment: { id: "top", snippet: { textDisplay: "d", authorDisplayName: "t", authorChannelId: { value: "UC1" }, publishedAt: "2024-02-01T00:00:00Z", likeCount: 12 } } },
      replies: { comments: [{ id: "r1", snippet: { textOriginal: "o" } }] },
    },
    "VID",
    "g"
  ),
  videos: [
    mapSearchVideo({ id: { videoId: "abc" }, snippet: { title: "t", channelId: "UC", publishedAt: "2024-01-02T03:04:05Z" } }),
    mapSearchVideo({ id: {} }),
    mapPlaylistVideo({ snippet: { resourceId: { videoId: "v9" } } }),
  ],
  impression: mapImpression("glab-x", { id: "v 1", gameTitle: "新作", recommend: false, comment: "c", createdAt: "2026-10-01T00:00:00Z" }),
});

write("slack", "blocks.json", { pick: buildFlowPickBlocks(), settings: buildFlowSettingsBlocks() });
write("flow", "premise.json", {
  premise: ["discussion", "improvement", "other", undefined].map((f) => ({ input: f ?? null, output: projectPremise(f) })),
  requires: ["discussion", "improvement", undefined].map((f) => ({ input: f ?? null, output: requiresProjectTitle(f) })),
  promptContract: DIAGNOSTIC_PROMPT_CONTRACT,
});
console.log("wrote crawler fixtures");
