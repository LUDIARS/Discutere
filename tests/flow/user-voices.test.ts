/**
 * ユーザーの声 (spec/feature/flow/user-voices.md) テスト。
 * - Steam: 同じゲームかの判定 / ストア検索 → appdetails でリリース判定
 * - Voluptas: 感想の取得 (認証ヘッダ・クエリ・HTTPS 強制) と匿名の ExternalUtterance 化
 * - 収集: リリース済み=Steam / 未リリース・見つからず=Voluptas / どちらも無し=none、ベクトル化の id
 * - 均等混合: ゲームごとに交互、足りない分は他で埋める、重複除去
 * - ゲーム単位の引き出し: gameSlug で引き、議題ベクトルに近い順
 * - 類似ゲーム指定の読み取り
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const { isSameGame, resolveSteamApp } = await import("../../src/flow/user-voices/steam-app.js");
const { mapImpression, fetchVoluptasImpressions, GLAB_ANONYMOUS_AUTHOR } = await import(
  "../../src/flow/user-voices/voluptas-impressions.js"
);
const { collectGameVoices, steamGameSlug, glabGameSlug } = await import("../../src/flow/user-voices/collect.js");
const { mixEvenly } = await import("../../src/flow/user-voices/mix.js");
const { listGameVoices } = await import("../../src/flow/user-voices/game-voices.js");
const { parseSimilarGames, themeGameRef } = await import("../../src/flow/user-voices/runtime.js");
const { createCore } = await import("../../src/core/index.js");
const { importExternalUtterances } = await import("../../src/crawler/sources/importer.js");
const { openAttributionStore } = await import("../../src/crawler/sources/attribution-store.js");

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });
}

// ── Steam ─────────────────────────────────────────────────────────────────────

{
  assert.equal(isSameGame("モンスターストライク", "モンスターストライクのガチャ天井を下げるか"), true);
  assert.equal(isSameGame("Monster Strike", "monster-strike"), true, "記号・空白・大小を無視");
  assert.equal(isSameGame("パズル&ドラゴンズ", "モンスターストライクの天井"), false, "別ゲームは採用しない");
  assert.equal(isSameGame("A", "A の話"), false, "短すぎる名前は採用しない");

  const urls: string[] = [];
  const fake = (released: boolean) =>
    (async (input: string | URL) => {
      const url = String(input);
      urls.push(url);
      if (url.includes("storesearch")) {
        return jsonResponse({ items: [{ id: 1, name: "パズル&ドラゴンズ" }, { id: 2, name: "モンスターストライク" }] });
      }
      return jsonResponse({ "2": { success: true, data: { name: "モンスターストライク", release_date: { coming_soon: !released } } } });
    }) as typeof fetch;

  const app = await resolveSteamApp({
    terms: ["モンスト", "モンスターストライク"],
    matchAgainst: "モンスターストライクのガチャ天井を下げるか",
    fetchImpl: fake(true),
  });
  assert.deepEqual(app, { appId: 2, name: "モンスターストライク", released: true }, "議題に名前が含まれる候補を採用");
  assert.ok(urls[0].includes("storesearch") && urls[1].includes("appids=2"));

  const unreleased = await resolveSteamApp({ terms: ["x"], matchAgainst: "モンスターストライク", fetchImpl: fake(false) });
  assert.equal(unreleased?.released, false, "coming_soon なら未リリース");

  const none = await resolveSteamApp({
    terms: ["a", "b", "c", "d"],
    matchAgainst: "全然違うゲーム",
    fetchImpl: (async () => jsonResponse({ items: [{ id: 9, name: "無関係" }] })) as typeof fetch,
    maxQueries: 2,
  });
  assert.equal(none, null, "一致しなければ null");
  console.log("  [ok] steam-app: 同じゲームの判定 + リリース判定");
}

// ── Voluptas ──────────────────────────────────────────────────────────────────

{
  const u = mapImpression("glab-x", {
    id: "v1",
    gameTitle: "新作",
    recommend: false,
    comment: "操作が重い",
    createdAt: "2026-10-01T00:00:00Z",
  });
  assert.equal(u.source, "glab");
  assert.equal(u.authorId, GLAB_ANONYMOUS_AUTHOR, "書き手は匿名の固定値");
  assert.equal(u.authorName, undefined);
  assert.deepEqual(u.signal, { votedUp: false });
  assert.equal(u.sourceUrl, "glab://impressions/v1");

  let seen: { url: string; auth: string | null } | null = null;
  const items = await fetchVoluptasImpressions({
    baseUrl: "http://localhost:3015",
    bearer: "tok",
    game: "新作",
    gameSlug: "glab-x",
    limit: 999,
    fetchImpl: (async (input: string | URL, init?: RequestInit) => {
      seen = { url: String(input), auth: new Headers(init?.headers).get("authorization") };
      return jsonResponse({
        ok: true,
        data: { impressions: [{ id: "v1", gameTitle: "新作", comment: "楽しい", createdAt: "x" }, { id: "v2", gameTitle: "新作", comment: " " }] },
      });
    }) as typeof fetch,
  });
  assert.equal(items.length, 1, "本文が空の感想は捨てる");
  assert.equal(seen!.auth, "Bearer tok");
  assert.ok(seen!.url.includes("/api/personas/impressions") && seen!.url.includes("limit=200"), "上限 200 に丸める");
  await assert.rejects(
    fetchVoluptasImpressions({ baseUrl: "http://example.com", bearer: "t", game: "g", gameSlug: "s", limit: 1 }),
    /HTTPS/,
    "loopback 以外は HTTPS を強制"
  );
  console.log("  [ok] voluptas-impressions: 匿名化 + 認証 + HTTPS 強制");
}

// ── 収集の分岐 ────────────────────────────────────────────────────────────────

{
  const game = themeGameRef("「新作」の操作感");
  assert.ok(game.searchTerms[0] === "新作", "「」の明示名を先に検索");
  const embedded: string[][] = [];
  const base = {
    core: {} as never,
    workspaceId: "knowledge",
    fetchSteam: async (_app: unknown, slug: string) => [
      { source: "steam" as const, nativeId: "r1", gameSlug: slug, threadKey: "1", content: "面白い", postedAt: 1, authorId: "S", sourceUrl: "u" },
    ],
    fetchGlab: async (_g: unknown, slug: string) => [
      { source: "glab" as const, nativeId: "v1", gameSlug: slug, threadKey: "新作", content: "重い", postedAt: 1, authorId: "anonymous", sourceUrl: "g" },
    ],
    importItems: (_c: unknown, items: unknown[]) => items.length,
    embed: async (_c: unknown, ids: string[]) => {
      embedded.push(ids);
      return ids.length;
    },
  };

  const released = await collectGameVoices(game, {
    ...base,
    resolveSteam: async () => ({ appId: 7, name: "新作", released: true }),
  });
  assert.equal(released.channel, "steam");
  assert.equal(released.gameSlug, steamGameSlug(7));
  assert.deepEqual(embedded.pop(), ["ext:steam:r1"], "取り込んだ声をベクトル化する");

  let sourceChecks = 0;
  const local = await collectGameVoices(game, {
    ...base, existingCount: () => 3,
    resolveSteam: async () => { sourceChecks++; return null; },
  });
  assert.equal(local.channel, "di");
  assert.equal(sourceChecks, 0, "Di データがあれば取得先を呼ばない");
  const youtube = await collectGameVoices(game, {
    ...base,
    resolveSteam: async () => ({ appId: 7, name: "新作", released: true }),
    fetchSteam: async () => [],
    fetchYoutube: async (_game, slug) => [{ source: "youtube", nativeId: "y1", gameSlug: slug, threadKey: "video", content: "操作が楽しい", postedAt: 1, authorId: "anonymous" }],
  });
  assert.equal(youtube.channel, "youtube", "Steam が 0 件なら YouTube を使う");

  const unreleased = await collectGameVoices(game, {
    ...base,
    resolveSteam: async () => ({ appId: 7, name: "新作", released: false }),
  });
  assert.equal(unreleased.channel, "glab", "未リリースなら Voluptas の感想");
  assert.equal(unreleased.unreleased, true);
  assert.equal(unreleased.gameSlug, glabGameSlug(game.title));
  assert.deepEqual(embedded.pop(), ["ext:glab:v1"]);

  const notFound = await collectGameVoices(game, { ...base, resolveSteam: async () => null, fetchGlab: undefined });
  assert.equal(notFound.channel, "none", "Steam に無く Voluptas 未設定なら none");
  assert.equal(notFound.gameSlug, null);

  const warnings: string[] = [];
  const failing = await collectGameVoices(game, {
    ...base,
    resolveSteam: async () => {
      throw new Error("network");
    },
    fetchGlab: async () => {
      throw new Error("401");
    },
    warn: (m) => warnings.push(m),
  });
  assert.equal(failing.channel, "none", "失敗しても止めない");
  assert.equal(warnings.length, 2);
  console.log("  [ok] collect: Steam / Voluptas / none の分岐 + ベクトル化 + 失敗時の継続");
}

// ── 均等混合 ──────────────────────────────────────────────────────────────────

{
  const key = (s: string) => s;
  assert.deepEqual(mixEvenly([["a1", "a2", "a3"], ["b1", "b2"], ["c1"]], 6, key), ["a1", "b1", "c1", "a2", "b2", "a3"]);
  assert.deepEqual(mixEvenly([["a1", "a2", "a3", "a4"], ["b1"]], 4, key), ["a1", "b1", "a2", "a3"], "足りない分は他で埋める");
  assert.deepEqual(
    mixEvenly([["x", "a"], ["x", "b"]], 3, key),
    ["x", "b", "a"],
    "同じ本文は 1 回だけ (重複したゲームはその巡で次の声を出す)"
  );
  assert.deepEqual(mixEvenly([[], []], 3, key), []);
  console.log("  [ok] mixEvenly: 交互 + 埋め合わせ + 重複除去");
}

// ── ゲーム単位の引き出し (一時 KG) ────────────────────────────────────────────

{
  const dir = path.resolve(".tmp/user-voices");
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  const core = createCore(path.join(dir, "db.kuzu"), path.join(dir, "events.jsonl"));
  const attribution = openAttributionStore(path.join(dir, ".attribution.sqlite"));
  try {
    const slug = steamGameSlug(7);
    const items = ["ガチャが渋い", "周回が楽しい", "グラフィックが綺麗"].map((content, i) => ({
      source: "steam" as const,
      nativeId: `r${i}`,
      gameSlug: slug,
      threadKey: "7",
      content,
      postedAt: 1000 + i,
      authorId: `S${i}`,
      sourceUrl: `https://example/r${i}`,
    }));
    importExternalUtterances(core, items, { workspaceId: "knowledge", attribution });
    // r1 を議題ベクトルに最も近くする。
    const vec = (x: number, y: number) => [x, y];
    core.vectors.registerEmbedding({ workspaceId: "knowledge", nodeType: "utterance", nodeId: "ext:steam:r0", vector: vec(0, 1) });
    core.vectors.registerEmbedding({ workspaceId: "knowledge", nodeType: "utterance", nodeId: "ext:steam:r1", vector: vec(1, 0) });

    const ranked = listGameVoices({ core, attribution, workspaceId: "knowledge", gameSlug: slug, limit: 3, queryVector: vec(1, 0.1) });
    assert.deepEqual(
      ranked.map((v) => v.content),
      ["周回が楽しい", "ガチャが渋い", "グラフィックが綺麗"],
      "ベクトルのある声を近い順、未ベクトル化は後ろ"
    );
    assert.equal(ranked[0].source, "steam");
    assert.equal(listGameVoices({ core, attribution, workspaceId: "knowledge", gameSlug: "steam-999", limit: 3 }).length, 0);
    assert.equal(listGameVoices({ core, attribution, workspaceId: "knowledge", gameSlug: slug, limit: 2 }).length, 2);
  } finally {
    attribution.close();
    core.close?.();
  }
  console.log("  [ok] listGameVoices: gameSlug で引き、議題ベクトルに近い順");
}

// ── 類似ゲーム指定の読み取り ──────────────────────────────────────────────────

{
  assert.deepEqual(parseSimilarGames("類似ゲーム:「モンスト」「パズドラ」"), ["モンスト", "パズドラ"]);
  assert.deepEqual(parseSimilarGames("ガチャ天井を下げるか"), []);
  assert.deepEqual(parseSimilarGames(undefined), []);
  console.log("  [ok] parseSimilarGames");
}

console.log("user-voices tests: all passed");
