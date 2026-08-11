/**
 * 外部の声検索コア — キーワード recall + ベクトル rerank のハイブリッド。
 *
 * 検索の 3 段構え (spec/feature/voice-rag-hybrid.md):
 *   1. recall: 議題語を分解 (extractKeyTerms) → ゲーム名/用語の別名展開
 *      (buildAliasGroups + Ludus 中央辞書) → SQL LIKE で候補集めここまでは従来通り。
 *   2. rerank: config.embedding.enabled かつクエリ埋め込みがキャッシュ済みなら、
 *      候補のベクトル (embeddings テーブル、offline 構築) と cosine を取り、
 *      キーワード順位と RRF 融合する。未キャッシュ時はキーワード順のまま返し、
 *      裏でクエリ埋め込みを温める (次回呼び出しからハイブリッド化)。候補の
 *      ベクトルが一部しか無い間も、索引有無の偏りを避けてキーワード順へ degrade する。
 *   3. diversify: 近似重複を畳み (bigram Jaccard)、MMR で「関連度 × 多様性」の
 *      上位 limit 件を選ぶ。ほぼ同文のコピペ群で prompt 枠が埋まるのを防ぐ。
 *
 * 同期 interface (listRelevantExternalVoices) を壊さないため、非同期が必要なのは
 * クエリ埋め込みの温めだけに隔離してある (query-embed-cache)。
 */

import type { createCore } from "../core/index.js";
import { getConfig } from "../config.js";
import { createOpenAiCompatEmbedder, type EmbeddingClient } from "../core/vectors/embedder.js";
import {
  getCachedQueryVector,
  normalizeQueryText,
  warmQueryEmbedding,
} from "../core/vectors/query-embed-cache.js";
import { fetchVectorsByNodeIds } from "../core/vectors/vector-search.js";
import { cosineSimilarity } from "../core/vectors/vector-math.js";
import {
  dedupeNearDuplicates,
  mmrSelect,
  rrfFuse,
  type MmrCandidate,
} from "../core/vectors/hybrid-rank.js";
import { ensureReactionTables, getOpinionScore } from "../discord-hook/reactions.js";
import { listExcludedIds } from "../core/noise/exclusions.js";
import { extractKeyTerms } from "./keyword-terms.js";
import { buildAliasGroups, expandAliases } from "./game-aliases.js";
import { LUDUS_TERM_ALIAS_GROUPS } from "./ludus-term-aliases.js";

type Core = ReturnType<typeof createCore>;

/** 関連語が無い時に opinion で拾う直近件数。 */
const RECENT_CAP = 300;
/** LIKE 一致候補の上限 (早期打ち切りで全件 LIKE でも軽い)。 */
const KEYWORD_CAP = 2000;
/** 別名展開後の検索語上限 (LIKE 句の暴発防止。用語辞書分をやや広めに取る)。 */
const EXPANDED_TERMS_MAX = 16;
/** ベクトル rerank に回す候補上限 (vector_json の JSON.parse コスト上限)。 */
const RERANK_CAP = 1000;
/** RRF 融合の系別重み (キーワード : ベクトル)。 */
const RRF_WEIGHTS = [1.0, 1.0] as const;
/** MMR の関連度配分 (0.7 = 関連度寄り)。 */
const MMR_LAMBDA = 0.7;

export interface ScoredVoiceRow {
  id: string;
  speakerId: string | null;
  rawContent: string;
  /** キーワード合成スコア (hits*10 + opinion)。順序はハイブリッド融合順が優先。 */
  score: number;
}

let embedderSingleton: EmbeddingClient | null = null;
function getEmbedder(): EmbeddingClient {
  if (!embedderSingleton) {
    embedderSingleton = createOpenAiCompatEmbedder(getConfig().embedding);
  }
  return embedderSingleton;
}

/** 議題語 → 別名展開済み検索語 (ゲーム名 + Ludus 用語辞書)。 */
export function buildSearchTerms(core: Core, workspaceId: string, terms: readonly string[]): string[] {
  let keyTerms = extractKeyTerms(terms);
  if (keyTerms.length === 0) return keyTerms;
  try {
    const titles = (
      core.client.raw
        .prepare("SELECT title FROM games WHERE workspace_id = ?")
        .all(workspaceId) as Array<{ title: string }>
    ).map((g) => g.title);
    keyTerms = expandAliases(
      keyTerms,
      buildAliasGroups(titles, LUDUS_TERM_ALIAS_GROUPS),
      EXPANDED_TERMS_MAX
    );
  } catch {
    // games 取得に失敗しても分解済みの語で続行 (別名拡張は best-effort)。
  }
  return keyTerms;
}

type VoiceRow = { id: string; speaker_id: string | null; raw_content: string };

function fetchCandidates(core: Core, workspaceId: string, keyTerms: readonly string[]): VoiceRow[] {
  if (keyTerms.length === 0) {
    return core.client.raw
      .prepare(
        `SELECT id, speaker_id, raw_content
           FROM utterances
          WHERE workspace_id = ? AND speaker_id LIKE 'ext:%'
          ORDER BY posted_at DESC LIMIT ?`
      )
      .all(workspaceId, RECENT_CAP) as VoiceRow[];
  }
  // LIKE のワイルドカード (% _ \) をエスケープして部分一致に倒す。
  const escapeLike = (t: string): string => t.replace(/[\\%_]/g, (c) => `\\${c}`);
  const likeClauses = keyTerms.map(() => `raw_content LIKE ? ESCAPE '\\'`).join(" OR ");
  return core.client.raw
    .prepare(
      `SELECT id, speaker_id, raw_content
         FROM utterances
        WHERE workspace_id = ? AND speaker_id LIKE 'ext:%' AND (${likeClauses})
        LIMIT ?`
    )
    .all(workspaceId, ...keyTerms.map((t) => `%${escapeLike(t)}%`), KEYWORD_CAP) as VoiceRow[];
}

/**
 * 外部の声を検索し、融合順に上位 limit 件を返す。
 * 同期関数 — ベクトル rerank はクエリ埋め込みがキャッシュ済みの時だけ効く。
 * @implements SPEC-VOICE-RAG-HYBRID-SEARCH
 */
export function searchExternalVoiceRows(
  core: Core,
  workspaceId: string,
  terms: readonly string[],
  limit: number,
  warn: (msg: string) => void = (m) => console.warn(`[voice-search] ${m}`),
  options: { useEmbedding?: boolean } = {}
): ScoredVoiceRow[] {
  if (limit <= 0) return [];
  const keyTerms = buildSearchTerms(core, workspaceId, terms);

  // opinion_scores はリアクション系テーブル。boot 前/単体呼び出しでも落ちないよう冪等保証。
  ensureReactionTables(core.client.raw);
  const excluded = listExcludedIds(core.client.raw);
  const rows = fetchCandidates(core, workspaceId, keyTerms).filter((r) => !excluded.has(r.id));
  if (rows.length === 0) return [];

  // キーワード合成スコア (従来互換): 一致語数を主、opinion (合意/支持) を従。
  const scored = rows
    .map((r) => {
      const lc = (r.raw_content ?? "").toLowerCase();
      const hits = keyTerms.reduce((n, t) => (lc.includes(t) ? n + 1 : n), 0);
      const opinion = getOpinionScore(core.client.raw, r.id);
      return { r, score: hits * 10 + opinion };
    })
    // 関連語ヒットも支持スコアも無い声は議論を薄めるので落とす。
    .filter((x) => x.score > 0)
    .sort((a, b) => b.score - a.score);
  if (scored.length === 0) return [];

  const embeddingCfg = getConfig().embedding;
  let ordered = scored;
  let vectors: Map<string, number[]> | null = null;

  if (options.useEmbedding !== false && embeddingCfg.enabled && terms.length > 0) {
    const queryText = normalizeQueryText(terms);
    const queryVec = getCachedQueryVector(core.client.raw, embeddingCfg.model, queryText);
    if (queryVec) {
      const rerankPool = scored.slice(0, RERANK_CAP);
      const candidateVectors = fetchVectorsByNodeIds(core.client, {
        workspaceId,
        nodeType: "utterance",
        nodeIds: rerankPool.map((x) => x.r.id),
      });
      // 部分索引では「ベクトル有り」自体が加点になり、強いキーワード候補を落とす。
      // rerank 対象が揃うまではキーワード順へ degrade する。
      const hasCompleteCoverage = rerankPool.every((x) => candidateVectors.has(x.r.id));
      if (hasCompleteCoverage) {
        vectors = candidateVectors;
        const vectorRank = rerankPool
          .map((x) => {
            const vector = candidateVectors.get(x.r.id);
            return vector ? { id: x.r.id, sim: cosineSimilarity(queryVec, vector) } : null;
          })
          .filter((x): x is { id: string; sim: number } => x !== null)
          .sort((a, b) => b.sim - a.sim)
          .map((x) => x.id);
        const keywordRank = scored.map((x) => x.r.id);
        const fused = rrfFuse([keywordRank, vectorRank], RRF_WEIGHTS);
        ordered = [...scored].sort(
          (a, b) => (fused.get(b.r.id) ?? 0) - (fused.get(a.r.id) ?? 0)
        );
      }
    } else {
      // 初回 miss: 裏で温めて次回からハイブリッド化 (今回はキーワード順で返す)。
      void warmQueryEmbedding(core.client.raw, getEmbedder(), embeddingCfg.model, queryText, warn);
    }
  }

  // 近似重複を畳んでから MMR で多様化して limit 件に絞る。
  const deduped = dedupeNearDuplicates(
    ordered.map((x) => ({ id: x.r.id, content: x.r.raw_content ?? "", entry: x }))
  );
  const byId = new Map(deduped.map((d) => [d.id, d.entry]));
  let pickedIds: string[];
  if (vectors && vectors.size > 0) {
    const maxFusedRank = deduped.length;
    const candidates: MmrCandidate[] = deduped.map((d, i) => ({
      id: d.id,
      // MMR の relevance は順位ベースで 1..0 に正規化 (スコアのスケール非依存)。
      relevance: (maxFusedRank - i) / maxFusedRank,
      vector: vectors.get(d.id),
    }));
    pickedIds = mmrSelect(candidates, limit, MMR_LAMBDA).map((c) => c.id);
  } else {
    pickedIds = deduped.slice(0, limit).map((d) => d.id);
  }

  return pickedIds
    .map((id) => byId.get(id))
    .filter((x): x is NonNullable<typeof x> => Boolean(x))
    .map((x) => ({
      id: x.r.id,
      speakerId: x.r.speaker_id,
      rawContent: x.r.raw_content ?? "",
      score: x.score,
    }));
}
