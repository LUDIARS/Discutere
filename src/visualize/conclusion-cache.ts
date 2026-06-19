/**
 * 結論一覧 (まとめ) の読み取りキャッシュ (conclusion_cache, discutere.db)。
 *
 * 学習ビューの「結論」タブは従来 `listConclusions` (KG 481MB を開いて行ごとにサブクエリ) +
 * `listFlowConclusions` を毎リクエストで実行していて重かった。ここでは:
 *   - 議論が収束するたびに `upsertConclusionCache` で 1 行 write-through 更新する
 *     (発話数 = discussionVolume は手元の値で即時、KG 非依存)。
 *   - 一覧閲覧 (`/learning/conclusions`) は `listCachedConclusions` でこのテーブルだけ読む。
 *   - 話題に紐づく外部クロール材料件数 (materialCount) は KG を引く重い計算なので、
 *     `recomputeMaterialCounts` で別途 (build:conclusion-cache) まとめて算出・更新する。
 *
 * 論述データ詳細 / md エクスポートは従来通りグラフ (KG) を引く (本キャッシュは一覧専用)。
 */

import type Database from "better-sqlite3";

import type { ConclusionKind, ConclusionSummary } from "./conclusions.js";
import { listFlowConclusions } from "./flow-conclusions.js";

/** materialCount 未算出マーカー (KG 別途計算で埋める)。 */
export const MATERIAL_COUNT_UNCOMPUTED = -1;

interface CacheRow {
  gap_id: string;
  kind: string;
  session_id: string;
  title: string;
  conclusion: string | null;
  utterance_count: number;
  aufhebung_count: number;
  discussion_volume: number;
  material_count: number;
  updated_at: number;
}

const UPSERT_SQL = `
  INSERT INTO conclusion_cache
    (gap_id, kind, session_id, title, conclusion, utterance_count, aufhebung_count,
     discussion_volume, material_count, updated_at, cached_at)
  VALUES
    (@gapId, @kind, @sessionId, @title, @conclusion, @utteranceCount, @aufhebungCount,
     @discussionVolume, @materialCount, @updatedAt, @cachedAt)
  ON CONFLICT(gap_id) DO UPDATE SET
    kind = excluded.kind,
    session_id = excluded.session_id,
    title = excluded.title,
    conclusion = excluded.conclusion,
    utterance_count = excluded.utterance_count,
    aufhebung_count = excluded.aufhebung_count,
    discussion_volume = excluded.discussion_volume,
    -- materialCount は重い KG 計算なので、呼び出し側が算出値 (>=0) を渡した時だけ上書きし、
    -- 未算出 (-1, = 収束 write-through) では既存値を温存する。
    material_count = CASE
      WHEN excluded.material_count >= 0 THEN excluded.material_count
      ELSE conclusion_cache.material_count
    END,
    updated_at = excluded.updated_at,
    cached_at = excluded.cached_at
`;

export interface UpsertOptions {
  /** 議論ボリューム。未指定なら summary.utteranceCount を使う。 */
  discussionVolume?: number;
  /** 外部クロール材料件数。未指定/未算出は温存 (既存値を壊さない)。 */
  materialCount?: number;
  /** テスト用に注入する現在時刻。 */
  now?: number;
}

/** 1 結論をキャッシュへ upsert する (write-through)。 */
export function upsertConclusionCache(
  db: Database.Database,
  summary: ConclusionSummary,
  opts: UpsertOptions = {}
): void {
  const now = opts.now ?? Date.now();
  db.prepare(UPSERT_SQL).run({
    gapId: summary.gapId,
    kind: summary.kind,
    sessionId: summary.sessionId,
    title: summary.title,
    conclusion: summary.conclusion,
    utteranceCount: summary.utteranceCount,
    aufhebungCount: summary.aufhebungCount,
    discussionVolume: opts.discussionVolume ?? summary.utteranceCount,
    materialCount: opts.materialCount ?? MATERIAL_COUNT_UNCOMPUTED,
    updatedAt: summary.updatedAt,
    cachedAt: now,
  });
}

/**
 * 収束時 (flow/conclusion.ts) の write-through 専用ヘルパ。
 * KG を一切引かず、手元の発話数/止揚数だけでキャッシュ行を更新する。
 */
export function writeThroughFlowConclusion(
  db: Database.Database,
  params: {
    sessionId: string;
    title: string;
    /** 収束まとめ本文 (【収束】プレフィックス除去済 / 未収束は null)。 */
    conclusion: string | null;
    utteranceCount: number;
    aufhebungCount: number;
    updatedAt?: number;
  }
): void {
  const now = params.updatedAt ?? Date.now();
  upsertConclusionCache(
    db,
    {
      gapId: `flow:${params.sessionId}`,
      kind: "flow",
      sessionId: params.sessionId,
      title: params.title,
      conclusion: params.conclusion,
      utteranceCount: params.utteranceCount,
      aufhebungCount: params.aufhebungCount,
      updatedAt: now,
    },
    { discussionVolume: params.utteranceCount, now }
  );
}

function rowToSummary(r: CacheRow): ConclusionSummary {
  return {
    gapId: r.gap_id,
    kind: r.kind as ConclusionKind,
    sessionId: r.session_id,
    title: r.title,
    conclusion: r.conclusion,
    utteranceCount: r.utterance_count,
    aufhebungCount: r.aufhebung_count,
    updatedAt: r.updated_at,
    discussionVolume: r.discussion_volume,
    materialCount: r.material_count,
  };
}

/** キャッシュ済み結論を新しい順に一覧する (KG 非タッチ)。 */
export function listCachedConclusions(db: Database.Database, limit = 100): ConclusionSummary[] {
  const rows = db
    .prepare(
      `SELECT gap_id, kind, session_id, title, conclusion, utterance_count, aufhebung_count,
              discussion_volume, material_count, updated_at
         FROM conclusion_cache
        ORDER BY updated_at DESC
        LIMIT ?`
    )
    .all(limit) as CacheRow[];
  return rows.map(rowToSummary);
}

/** キャッシュ行数 (fallback 判定用 — 0 件なら live build に倒す)。 */
export function conclusionCacheCount(db: Database.Database): number {
  const row = db.prepare("SELECT COUNT(*) AS n FROM conclusion_cache").get() as { n: number };
  return row.n;
}

/**
 * 新フロー結論をキャッシュへ取り込んで「追いついた」状態を保証する (自己修復・KG 非依存)。
 *
 * write-through (収束時) だけだと、過去分が未バックフィルのまま 1 件 write-through された瞬間に
 * 一覧が「キャッシュに在る分だけ」に痩せてしまう。ここでキャッシュ件数が flow_conclusion 総数に
 * 満たない時だけ全 flow 結論を upsert し直す (発話数は反映、materialCount は -1 のまま温存)。
 * 既に追いついていれば COUNT 2 回だけで即 return する (一覧経路を重くしない)。
 *
 * 旧 design_gaps (legacy) 結論と materialCount は KG を引くため対象外。
 * それらは `npm run build:conclusion-cache` で別途バックフィルする。
 */
export function ensureConclusionCacheFresh(db: Database.Database): void {
  const flowTotal = (
    db.prepare("SELECT COUNT(*) AS n FROM flow_conclusion").get() as { n: number }
  ).n;
  if (conclusionCacheCount(db) >= flowTotal) return;
  const flows = listFlowConclusions(db, Number.MAX_SAFE_INTEGER);
  const run = db.transaction((list: ConclusionSummary[]) => {
    for (const s of list) upsertConclusionCache(db, s, { discussionVolume: s.utteranceCount });
  });
  run(flows);
}

/** 1 行の materialCount を更新する (KG 別途計算の書き戻し)。 */
export function setMaterialCount(db: Database.Database, gapId: string, count: number): void {
  db.prepare("UPDATE conclusion_cache SET material_count = ? WHERE gap_id = ?").run(count, gapId);
}

/**
 * 全キャッシュ行について materialCount を `countMaterial` で算出して書き戻す (別途計算)。
 * `countMaterial` は KG を引く重い関数 (build スクリプトが配線、テストは stub を注入)。
 * @returns 更新した行数。
 */
export function recomputeMaterialCounts(
  db: Database.Database,
  countMaterial: (summary: ConclusionSummary) => number,
  opts: { onlyMissing?: boolean } = {}
): number {
  const rows = listCachedConclusions(db, Number.MAX_SAFE_INTEGER);
  const targets = opts.onlyMissing ? rows.filter((r) => (r.materialCount ?? -1) < 0) : rows;
  const update = db.prepare("UPDATE conclusion_cache SET material_count = ? WHERE gap_id = ?");
  const run = db.transaction((list: ConclusionSummary[]) => {
    for (const r of list) {
      let count: number;
      try {
        count = countMaterial(r);
      } catch {
        continue; // 1 件の失敗で全体を止めない
      }
      if (Number.isFinite(count) && count >= 0) update.run(count, r.gapId);
    }
  });
  run(targets);
  return targets.length;
}
