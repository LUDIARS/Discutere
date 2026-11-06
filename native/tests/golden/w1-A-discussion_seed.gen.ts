// src/discussion-seed/{genres,store-trends}.ts の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-A-discussion_seed.gen.ts
// store-trends は fetch を偽の応答に差し替えて記録する (本物の Steam には繋がない)。
import fs from "node:fs";
import path from "node:path";
import { GENRE_SEEDS, buildGenreTopic, pickGenreSeed } from "../../../src/discussion-seed/genres.js";
import { fetchStoreTrendTopics } from "../../../src/discussion-seed/store-trends.js";

const genreTopics = GENRE_SEEDS.map((s) => buildGenreTopic(s));
const noAngle = buildGenreTopic({ genre: "テスト" });
const picks = [0, 1, 16, 17, 18, -1, -17, -18, 1000].map((i) => ({ index: i, genre: pickGenreSeed(i).genre }));

const body = JSON.stringify({ top_sellers: { items: [{ name: " Game A " }, { name: 3 }, { name: "" }, {}, null, { name: "ゲームB" }, { name: "C" }] } });
const trends: unknown[] = [];
for (const [label, response, limit] of [
  ["ok", body, 5], ["ok-limit-1", body, 1], ["ok-limit-0", body, 0], ["http-500", null, 5],
  ["broken-json", "{", 5], ["no-items", "{\"top_sellers\":{}}", 5], ["fallback-limit-3", null, 3], ["fallback-limit-neg", null, -1],
] as Array<[string, string | null, number]>) {
  globalThis.fetch = (async () => (response === null ? new Response("err", { status: 500 }) : new Response(response, { status: 200 }))) as typeof fetch;
  trends.push({ label, response, limit, topics: await fetchStoreTrendTopics(limit) });
}
const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "discussion_seed");
fs.writeFileSync(path.join(dir, "seeds.json"), JSON.stringify({ genreTopics, noAngle, picks, trends }, null, 2) + "\n");
console.log(`wrote ${trends.length} trend cases`);
