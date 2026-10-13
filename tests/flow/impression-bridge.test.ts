/**
 * Voluptas → Discutere の感想受け取り (spec/feature/flow/voluptas-impression-push.md) テスト。
 * - 匿名でない項目を含む感想は 1 件ごと拒否、形の壊れた感想は invalid、件数上限は 400
 * - 取込は匿名 (authorId=anonymous)・ゲーム単位 slug・dedup は importer に任せる
 * - 受け口の認証 (固定トークン: 未設定 503 / 不一致 401 / 一致 200) と本文エラー 400
 * - Voluptas に取りに行けないとき、届いていた感想で声を集めたことにする (collect の fallback)
 */

import assert from "node:assert/strict";

const { parseImpressionBatch, ingestImpressions, IMPRESSION_PUSH_MAX, ImpressionBatchError } = await import(
  "../../src/persona-bridge/impression-intake.js"
);
const { createImpressionBridgeRoutes } = await import("../../src/api/impression-bridge-routes.js");
const { collectGameVoices, glabGameSlug } = await import("../../src/flow/user-voices/collect.js");
const { VoluptasUnavailable } = await import("../../src/flow/user-voices/voluptas-impressions.js");

const good = {
  id: "imp-1",
  gameTitle: "テストゲーム",
  recommend: true,
  polarity: "positive",
  comment: "周回が楽しい",
  createdAt: "2026-10-01T00:00:00.000Z",
};

// ── 検査 ──────────────────────────────────────────────────────────
{
  const r = parseImpressionBatch({
    impressions: [
      good,
      { ...good, id: "imp-2", userId: "u-123" },
      { ...good, id: "imp-3", authorName: "山田" },
      { ...good, id: "imp-4", comment: "" },
      { ...good, id: "imp-5", createdAt: "not-a-date" },
      "文字列",
      { id: "imp-6", gameTitle: "テストゲーム", comment: "最低限", createdAt: "2026-10-02T00:00:00Z" },
    ],
  });
  assert.deepEqual(r.impressions.map((i) => i.id), ["imp-1", "imp-6"]);
  assert.equal(r.skipped.non_anonymous_field, 2, "書き手の項目を含む感想は拒否");
  assert.equal(r.skipped.invalid, 3);
  assert.equal(r.impressions[1].recommend, null);

  assert.throws(() => parseImpressionBatch({}), ImpressionBatchError);
  assert.throws(
    () => parseImpressionBatch({ impressions: Array.from({ length: IMPRESSION_PUSH_MAX + 1 }, () => good) }),
    ImpressionBatchError
  );
  console.log("  [ok] parseImpressionBatch: 匿名性・形・件数");
}

// ── 取込 ──────────────────────────────────────────────────────────
{
  const imported: any[] = [];
  const r = await ingestImpressions([good, { ...good, id: "imp-9", gameTitle: "別のゲーム" }], {
    importItems: (items) => {
      imported.push(...items);
      return items.length;
    },
    embed: async (ids) => ids.length,
  });
  assert.equal(r.received, 2);
  assert.equal(r.imported, 2);
  assert.equal(r.embedded, 2);
  assert.ok(imported.every((u) => u.source === "glab" && u.authorId === "anonymous"), "書き手は常に匿名");
  assert.equal(imported[0].gameSlug, glabGameSlug("テストゲーム"));
  assert.equal(r.games[glabGameSlug("別のゲーム")], 1);

  const failed = await ingestImpressions([good], {
    importItems: () => 1,
    embed: async () => {
      throw new Error("down");
    },
  });
  assert.equal(failed.embedded, 0, "埋め込み失敗は取込を止めない");
  assert.equal((await ingestImpressions([], { importItems: () => 99 })).imported, 0);
  console.log("  [ok] ingestImpressions");
}

// ── 受け口 ────────────────────────────────────────────────────────
{
  const TOKEN = "t".repeat(40);
  const ingested: string[] = [];
  const app = createImpressionBridgeRoutes({
    token: () => TOKEN,
    serviceAudience: () => "discutere",
    withIngest: async (run) => {
      await run({
        importItems: (items) => {
          ingested.push(...items.map((i) => i.nativeId));
          return items.length;
        },
      });
    },
  });
  const post = (body: unknown, auth?: string) =>
    app.request("/persona-bridge/impressions", {
      method: "POST",
      headers: { "content-type": "application/json", ...(auth ? { authorization: auth } : {}) },
      body: typeof body === "string" ? body : JSON.stringify(body),
    });

  assert.equal((await post({ impressions: [good] })).status, 401, "トークン無しは 401");
  assert.equal((await post({ impressions: [good] }, "Bearer wrong")).status, 401);
  const ok = await post({ impressions: [good, { ...good, id: "x", userId: "u" }] }, `Bearer ${TOKEN}`);
  assert.equal(ok.status, 200);
  const body = (await ok.json()) as any;
  assert.equal(body.imported, 1);
  assert.equal(body.skipped.non_anonymous_field, 1);
  assert.deepEqual(ingested, ["imp-1"]);
  assert.equal((await post("not json", `Bearer ${TOKEN}`)).status, 400);
  assert.equal((await post({ nope: 1 }, `Bearer ${TOKEN}`)).status, 400);

  const unconfigured = createImpressionBridgeRoutes({ token: () => "short", withIngest: async () => {} });
  const r503 = await unconfigured.request("/persona-bridge/impressions", {
    method: "POST",
    headers: { "content-type": "application/json", authorization: "Bearer short" },
    body: JSON.stringify({ impressions: [] }),
  });
  assert.equal(r503.status, 503, "短い固定トークンは未設定扱い");
  console.log("  [ok] POST /persona-bridge/impressions");
}

// ── 届いていた感想への fallback ────────────────────────────────────
{
  const baseDeps = {
    core: {} as any,
    workspaceId: "knowledge",
    resolveSteam: async () => null,
    fetchSteam: async () => [],
    importItems: () => 0,
  };
  const game = { title: "テストゲーム", searchTerms: ["テストゲーム"], matchAgainst: ["テストゲーム"] } as any;

  const down = await collectGameVoices(game, {
    ...baseDeps,
    fetchGlab: async () => {
      throw new VoluptasUnavailable();
    },
    storedGlabCount: (slug) => (slug === glabGameSlug("テストゲーム") ? 3 : 0),
  });
  assert.equal(down.channel, "glab", "取りに行けなくても届いていた感想を使う");
  assert.equal(down.collected, 3);
  assert.equal(down.gameSlug, glabGameSlug("テストゲーム"));

  const noFetch = await collectGameVoices(game, { ...baseDeps, storedGlabCount: () => 2 });
  assert.equal(noFetch.channel, "glab", "Voluptas 未設定でも届いていた感想を使う");

  const none = await collectGameVoices(game, { ...baseDeps, fetchGlab: async () => [], storedGlabCount: () => 0 });
  assert.equal(none.channel, "none");
  console.log("  [ok] collectGameVoices: 届いていた感想への fallback");
}

console.log("impression-bridge tests: all passed");
