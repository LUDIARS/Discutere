/**
 * データ調整 UX (admin) — spec/feature/crawler/EXTERNAL-SOURCES.md §13。
 *
 * 「タスク別に KG を分けて運用切替する」前提では、 取り込んだデータを見て・絞って・除外できる
 * UX が運用価値の中心になる。 admin dashboard の「データソース」面の API を提供する。
 *
 * - GET  /api/admin/datasources              — active KG + 宣言済み KG + source 別件数。
 * - GET  /api/admin/datasources/:source/utterances — 当該 source の取り込み発話一覧 (出所付き)。
 * - POST /api/admin/datasources/exclude      — 発話/source 単位の除外 (学習・議論から外す)。
 * - POST /api/admin/datasources/active-kg    — 次に開く KG を config に書き戻す (再起動で反映)。
 *
 * 個人マスクは §6 露出 serializer (maskedPersonaLabel) を通す。 生 ID は admin のみ可視。
 * 除外は既存 noise 除去 (utterance_exclusions) + .ingested 台帳に寄せ、 二重実装しない。
 */

import { existsSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";

import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { listKnowledgeGraphs } from "../core/kg-registry.js";
import { getConfig } from "../config.js";
import { openAttributionStore } from "../crawler/sources/attribution-store.js";
import { openIngestedStore } from "../crawler/sources/ingested-store.js";
import type { ExternalSource } from "../crawler/sources/types.js";
import { excludeUtterance, listExcludedIds } from "../core/noise/exclusions.js";
import { maskedPersonaLabel } from "../crawler/sources/persona.js";

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}

/** active KG (§12) を開く Core。 */
function openCore() {
  return createCore(resolveActiveKgPath(getConfig()));
}

export const datasourceRoutes = new Hono();

// active KG + 宣言済み KG 一覧 + source 別件数 (source_attribution 集計)。
datasourceRoutes.get("/admin/datasources", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const config = getConfig();
  const attribution = openAttributionStore();
  try {
    const sources = attribution.countsBySource().map((s) => ({
      source: s.source,
      count: s.count,
      gameSlugs: s.gameSlugs,
      lastImportedAt: s.lastImportedAt,
    }));
    return c.json({
      activeKg: config.discatier.activeKg ?? "default",
      knowledgeGraphs: listKnowledgeGraphs(config),
      sources,
    });
  } finally {
    attribution.close();
  }
});

// 当該 source の取り込み発話一覧 (本文抜粋 + ペルソナ名 + 出所 + 生 ID は admin のみ)。
datasourceRoutes.get("/admin/datasources/:source/utterances", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const source = c.req.param("source");
  const gameSlug = c.req.query("gameSlug") || null;
  const limit = Math.min(500, Math.max(1, Number(c.req.query("limit") ?? 200)));
  const core = openCore();
  const attribution = openAttributionStore();
  try {
    const raw = core.client.raw;
    const excluded = listExcludedIds(raw);
    const rows = attribution.listBySource(source, gameSlug, limit);
    const utterStmt = raw.prepare(
      "SELECT speaker_id, raw_content FROM utterances WHERE id = ?"
    );
    const items = rows.map((a) => {
      const u = utterStmt.get(a.utteranceId) as
        | { speaker_id: string | null; raw_content: string }
        | undefined;
      const content = u?.raw_content ?? "";
      return {
        utteranceId: a.utteranceId,
        // §6: 個人はペルソナ名へマスク、 出所 (source/sourceUrl) は開示。
        speaker: u?.speaker_id ? maskedPersonaLabel(u.speaker_id) : "外部の声",
        excerpt: content.length > 280 ? content.slice(0, 280) + "…" : content,
        source: a.source,
        sourceUrl: a.sourceUrl,
        gameSlug: a.gameSlug,
        // 生 ID は admin のみ可視 (本ルートは requireAdmin 済)。
        speakerId: u?.speaker_id ?? null,
        nativeId: a.nativeId,
        importedAt: a.importedAt,
        excluded: excluded.has(a.utteranceId),
      };
    });
    return c.json({ source, gameSlug, items });
  } finally {
    attribution.close();
    core.close();
  }
});

// 発話単位 or (source[, gameSlug]) 単位で取り込みを除外する。
// 既存 noise 除去 (utterance_exclusions) + .ingested 台帳に「除外済み」を残し再取り込みを防ぐ。
datasourceRoutes.post("/admin/datasources/exclude", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const body = await c.req
    .json<{ utteranceId?: string; source?: string; gameSlug?: string }>()
    .catch(() => ({}) as { utteranceId?: string; source?: string; gameSlug?: string });
  if (!body.utteranceId && !body.source) {
    return c.json({ error: "utteranceId or source required" }, 400);
  }
  const core = openCore();
  const attribution = openAttributionStore();
  const ingested = openIngestedStore();
  try {
    const raw = core.client.raw;
    let excludedCount = 0;
    // 除外対象の attribution 行を集める (発話単位 or source[/gameSlug] 単位)。
    const targets = body.utteranceId
      ? (() => {
          const a = attribution.get(body.utteranceId!);
          return a ? [a] : [{ utteranceId: body.utteranceId!, source: "", nativeId: "", sourceUrl: "", gameSlug: null, importedAt: 0 }];
        })()
      : attribution.listBySource(body.source!, body.gameSlug ?? null, 100_000);
    for (const a of targets) {
      excludeUtterance(raw, a.utteranceId, body.source ? `datasource:${body.source}` : "datasource");
      // 再取り込み防止: source/nativeId が分かれば .ingested に印を残す。
      if (a.source && a.nativeId) ingested.add(a.source as ExternalSource, a.nativeId);
      excludedCount += 1;
    }
    return c.json({ ok: true, excluded: excludedCount });
  } finally {
    ingested.close();
    attribution.close();
    core.close();
  }
});

// 次に開く KG (activeKg) を config ファイルに書き戻す。 即時切替はせず再起動が必要。
datasourceRoutes.post("/admin/datasources/active-kg", async (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const body = await c.req.json<{ id?: string }>().catch(() => ({}) as { id?: string });
  if (!body.id) return c.json({ error: "id required" }, 400);
  const config = getConfig();
  const known = listKnowledgeGraphs(config).map((kg) => kg.id);
  if (known.length > 0 && !known.includes(body.id)) {
    return c.json({ error: `unknown KG id: ${body.id}`, knownIds: known }, 400);
  }
  const configPath = path.resolve(process.env.DISCUTERE_CONFIG ?? "./discutere.config.json");
  try {
    const current = existsSync(configPath)
      ? (JSON.parse(readFileSync(configPath, "utf8")) as Record<string, unknown>)
      : {};
    const discatier = (current.discatier as Record<string, unknown>) ?? {};
    discatier.activeKg = body.id;
    current.discatier = discatier;
    writeFileSync(configPath, JSON.stringify(current, null, 2) + "\n", "utf8");
  } catch (err) {
    return c.json({ error: `config 書き込み失敗: ${(err as Error).message}` }, 500);
  }
  // 即時切替はしない (§12 — 無停止ホットスワップ非対応)。 再起動で反映。
  return c.json({ ok: true, activeKg: body.id, restartRequired: true });
});
