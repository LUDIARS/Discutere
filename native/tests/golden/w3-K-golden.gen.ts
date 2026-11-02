// w3-K (学習・情報ゲート・ユーザーの声・感想の受け口・経済グラフ・crawl チャンネル) の Node 実装の入出力を記録する。
// 実行 (import 時に data/discutere.db を開くモジュールがあるので一時ディレクトリで動かす):
//   tmp=$(mktemp -d) && cd "$tmp" && DATABASE_PATH="$tmp/x.db" /home/user/wt/w3-K/node_modules/.bin/tsx /home/user/wt/w3-K/native/tests/golden/w3-K-golden.gen.ts
// 出力: native/tests/fixtures/w3k/*.json と native/web/api/economy-graph.html (値はすべて架空)
import fs from "node:fs";
import path from "node:path";

import { toSlug } from "../../../src/ludus/economy-analyzer.js";
import { deriveSlug, resolveAutoCrawlSources, isAutoCrawlSource } from "../../../src/flow/learning-autocrawl.js";
import { evaluateInformationDensity } from "../../../src/flow/information-gate.js";
import { densityFeatures } from "../../../src/flow/density-blackbox.js";
import { glabGameSlug, describeVoiceReport, steamGameSlug } from "../../../src/flow/user-voices/collect.js";
import { themeGameRef, parseSimilarGames } from "../../../src/flow/user-voices/runtime.js";
import { parseImpressionBatch } from "../../../src/persona-bridge/impression-intake.js";
import {
  extractUrls,
  parseCrawlMessage,
  classifyCrawlUrl,
  slugFromTarget,
  formatCrawlSummary,
} from "../../../src/discord-hook/crawl-channel.js";
import { createEconomyGraphRoutes } from "../../../src/api/economy-graph-routes.js";
import type { LLMClient } from "../../../src/persona-engine/llm/client.js";
import type { ContextVoice } from "../../../src/flow/discussion-paper.js";

const here = path.dirname(new URL(import.meta.url).pathname);
const root = path.join(here, "..", "fixtures", "w3k");
fs.mkdirSync(root, { recursive: true });
const write = (name: string, value: unknown) => {
  fs.writeFileSync(path.join(root, name), JSON.stringify(value, null, 2) + "\n");
  console.log(`wrote ${name}`);
};

// ── slug ──
const slugInputs = [
  "Hello World",
  "  --Vampire Survivors!!  ",
  "モンスターストライク",
  "ローグライト",
  "Slay the Spire 2",
  "ポケモン Legends Z-A",
  "İstanbul Kart",
  "\u212Aelvin",
  "a\tb\n c",
  "",
  "   ",
  "ゲーム　全角空白　入り",
  "x".repeat(60),
  "あ".repeat(60),
  "😀絵文字😀ゲーム",
  "Ünïcödé Gämé",
];
write(
  "slugs.json",
  slugInputs.map((input) => ({ input, toSlug: toSlug(input), deriveSlug: deriveSlug(input), glabGameSlug: glabGameSlug(input) }))
);
write("auto_crawl_sources.json", {
  isAutoCrawlSource: ["niconico", "youtube", "steam", "website", "reddit", "x", "NICONICO"].map((s) => [s, isAutoCrawlSource(s)]),
  resolve: [
    { sources: ["niconico", "steam", "website"], key: null, out: resolveAutoCrawlSources(["niconico", "steam", "website"]) },
    { sources: ["niconico"], key: "k", out: resolveAutoCrawlSources(["niconico"], "k") },
    { sources: ["youtube", "niconico", "youtube"], key: null, out: resolveAutoCrawlSources(["youtube", "niconico", "youtube"]) },
    { sources: ["bogus", "niconico"], key: "k", out: resolveAutoCrawlSources(["bogus", "niconico"], "k") },
  ],
});

// ── 情報ゲート: プロンプトと応答の解釈 ──
const longVoice = "長".repeat(170);
const voiceSets: ContextVoice[][] = [
  [],
  [{ content: "楽しい", source: "niconico" }],
  [
    { content: longVoice, source: "steam" },
    { content: "😀".repeat(90), source: "youtube" },
    { content: "short", source: "steam", sourceUrl: "https://example.invalid/a" },
  ],
  Array.from({ length: 35 }, (_, i) => ({ content: `声${i}`, source: i % 2 ? "niconico" : "youtube" })),
];
const responses = [
  JSON.stringify({ density: "rich", covered: ["肯定", 3, "否定"], gaps: [] }),
  "前置き " + JSON.stringify({ density: "sparse", gaps: [{ aspect: " 否定的意見 ", reason: "偏り", query: "", source: "reddit" }, { aspect: "" }, "x", { aspect: "具体例", query: " q ", source: "bogus" }] }) + " 後書き",
  JSON.stringify({ density: "MODERATE", covered: "x", gaps: "y", proposedRule: { when: { op: "cmp", feature: "voiceCount", cmp: ">=", value: 8 } } }),
  "JSON ではない文章",
  "{ 壊れた: json }",
];
const evalCases: unknown[] = [];
for (const voices of voiceSets) {
  for (const tags of [[], ["機密", "運用"]]) {
    for (const text of responses) {
      let seen: { system?: string; prompt: string } | null = null;
      const llm: LLMClient = {
        async invoke(args) {
          seen = { system: args.system, prompt: args.prompt };
          return { ok: true, text };
        },
      };
      const ev = await evaluateInformationDensity({ theme: "周回設計", tags: tags as never, voices, llm });
      evalCases.push({ voices, tags, response: text, system: seen!.system, prompt: seen!.prompt, evaluation: ev });
    }
  }
}
write("information_gate.json", evalCases);
write(
  "density_features.json",
  voiceSets.map((voices) => ({ voices, tags: ["機密"], features: densityFeatures(voices, ["機密"] as never) }))
);

// ── ユーザーの声 ──
const themes = [
  "「新作」の操作感",
  "『モンスターストライク』と「パズドラ」のガチャ比較",
  "「a」短すぎ「」空「" + "長".repeat(41) + "」長すぎ",
  "ガチャ天井を下げるか",
  "「😀😀」",
];
write(
  "theme_game_refs.json",
  themes.map((theme) => ({ theme, ref: themeGameRef(theme) }))
);
write(
  "similar_games.json",
  ["類似ゲーム:「モンスト」「パズドラ」", "ガチャ天井を下げるか", "似たゲーム「A」"].map((text) => ({ text, out: parseSimilarGames(text) }))
);
const reports = [
  { title: "T", channel: "di", gameSlug: null, collected: 4, imported: 0, embedded: 0 },
  { title: "T", channel: "youtube", gameSlug: "youtube-glab-t", collected: 5, imported: 5, embedded: 0 },
  { title: "T", channel: "steam", gameSlug: steamGameSlug(7), appId: 7, appName: "新作", collected: 3, imported: 2, embedded: 1 },
  { title: "T", channel: "glab", gameSlug: "glab-t", unreleased: true, appId: 7, appName: "新作", collected: 3, imported: 3, embedded: 0 },
  { title: "T", channel: "glab", gameSlug: "glab-t", collected: 2, imported: 2, embedded: 2 },
  { title: "T", channel: "none", gameSlug: null, collected: 0, imported: 0, embedded: 0, voluptasDown: true },
  { title: "T", channel: "none", gameSlug: null, collected: 0, imported: 0, embedded: 0 },
];
write("voice_reports.json", reports.map((r) => ({ report: r, text: describeVoiceReport(r as never) })));

// ── 感想の受け口 ──
const good = { id: "imp-1", gameTitle: " テストゲーム ", recommend: true, polarity: "positive", comment: "周回が楽しい", createdAt: "2026-10-01T00:00:00.000Z" };
const batches: unknown[] = [
  { impressions: [good] },
  { impressions: [good, { ...good, id: "imp-2", userId: "u" }, { ...good, id: "a b" }, { ...good, id: "x".repeat(128) }, { ...good, id: "x".repeat(129) }] },
  { impressions: [{ ...good, gameTitle: "g".repeat(200) }, { ...good, gameTitle: "g".repeat(201) }, { ...good, gameTitle: "😀".repeat(100) }, { ...good, gameTitle: "😀".repeat(101) }] },
  { impressions: [{ ...good, comment: "c".repeat(10000) }, { ...good, comment: "c".repeat(10001) }, { ...good, comment: "   " }] },
  { impressions: [{ ...good, recommend: null, polarity: null }, { ...good, recommend: "yes" }, { ...good, polarity: 1 }, { id: "m", gameTitle: "g", comment: "c", createdAt: "2026-10-02T00:00:00Z" }] },
  { impressions: [{ ...good, createdAt: "not-a-date" }, { ...good, createdAt: 5 }, null, [], "s", 3] },
  { impressions: [] },
  { impressions: Array.from({ length: 200 }, (_, i) => ({ ...good, id: `i${i}` })) },
  { impressions: Array.from({ length: 201 }, (_, i) => ({ ...good, id: `i${i}` })) },
  {},
  { impressions: "x" },
  [],
  null,
];
write(
  "impression_batches.json",
  batches.map((body) => {
    try {
      return { body, ok: true, value: JSON.parse(JSON.stringify(parseImpressionBatch(body))) };
    } catch (e) {
      return { body, ok: false, error: (e as Error).message };
    }
  })
);

// ── crawl チャンネル ──
const texts = [
  "見て https://a.test/x これと http://b.test/y。",
  "URL なし",
  "HTTPS://Upper.test/Path), (http://p.test/q)",
  "「https://jp.test/あ」と【https://k.test/z】",
  "<https://angle.test/a> 'https://q.test/b' \"https://d.test/c\"",
  "https:// http://x",
  "末尾https://dot.test/a.,。、",
  "https://a.test/x　全角空白",
  "httpx://no.test https://ok.test/😀",
];
const messages = [
  "[sotc] https://a.test/x",
  "sotc: https://a.test/x https://a.test/y",
  "https://a.test/x",
  "[SoTc-2]   https://a.test/x",
  "[" + "a".repeat(64) + "] https://a.test/x",
  "[" + "a".repeat(65) + "] https://a.test/x",
  "slug:https://a.test/x",
  "slug: not-a-url https://a.test/x",
  "  [-bad] https://a.test/x  ",
  "Game: HTTP://a.test/x",
];
const urls = [
  "https://store.steampowered.com/app/1794680/Vampire_Survivors/",
  "https://steamcommunity.com/app/42",
  "https://store.steampowered.com/agecheck/app/10/",
  "https://store.steampowered.com/search/?app/1",
  "https://www.youtube.com/watch?v=dQw4w9WgXcQ",
  "https://www.youtube.com/watch?v=",
  "https://youtu.be/abc123XYZ_-",
  "https://youtu.be/",
  "https://m.youtube.com/shorts/abcdef",
  "https://youtube.com/live/abcde",
  "https://www.youtube.com/embed/abcdefg?x=1",
  "https://www.youtube.com/results?search_query=x",
  "https://www.reddit.com/r/gaming/comments/abc123/some_title/",
  "https://old.reddit.com/comments/XyZ9",
  "https://np.reddit.com/r/x/",
  "https://famitsu.com/news/x.html",
  "https://www.example.com/p",
  "http://Sub.Example.CO.jp:8080/a",
  "not a url",
];
write("crawl_channel.json", {
  extractUrls: texts.map((text) => ({ text, out: extractUrls(text) })),
  parseCrawlMessage: messages.map((text) => ({ text, out: parseCrawlMessage(text) })),
  classify: urls.map((url) => {
    const t = classifyCrawlUrl(url);
    return { url, target: t, slug: slugFromTarget(t) };
  }),
  summaries: [
    [
      { url: "https://a.test", kind: "website", gameSlug: "g", fetched: 1, imported: 1, skipped: 0 },
      { url: "https://b.test", kind: "steam", gameSlug: "steam-1", fetched: 0, imported: 0, skipped: 0, error: "boom" },
      { url: "https://c.test", kind: "youtube", gameSlug: "yt-x", fetched: 3, imported: 0, skipped: 3 },
    ],
    [],
  ].map((results) => ({ results, text: formatCrawlSummary(results as never) })),
});

// ── 経済グラフの HTML (画面は native/web に置く) ──
const app = createEconomyGraphRoutes({ workspaceId: "knowledge", apiKey: "" });
const SLUG_MARK = "__W3K_SLUG__";
const BASE_MARK = "__W3K_BASE__";
const res = await app.request(`/ludus/economy-graph/${SLUG_MARK}`, { headers: { host: BASE_MARK, "x-forwarded-proto": "p" } });
let html = await res.text();
html = html.replace(JSON.stringify(SLUG_MARK), "{{slugJson}}").replace(JSON.stringify(`p://${BASE_MARK}`), "{{apiBaseJson}}");
if (html.includes("__W3K_")) throw new Error("template markers not replaced");
fs.writeFileSync(path.join(here, "..", "..", "web", "api", "economy-graph.html"), html);
console.log("wrote web/api/economy-graph.html");
const samples = [
  { slug: "vampire-survivors", host: "localhost:3000", proto: undefined },
  { slug: "slay-the-spire", host: undefined, proto: "https" },
];
const htmlCases: unknown[] = [];
for (const s of samples) {
  const headers: Record<string, string> = {};
  if (s.host) headers.host = s.host;
  if (s.proto) headers["x-forwarded-proto"] = s.proto;
  const r = await app.request(`/ludus/economy-graph/${s.slug}`, { headers });
  htmlCases.push({ ...s, status: r.status, html: await r.text() });
}
write("economy_graph_html.json", htmlCases);
