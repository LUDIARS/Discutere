/**
 * C1 実在ユーザ採用ランナー (KG 集約 → 条件フィルタ → flow_persona upsert)。
 *
 * `scripts/persona-adopt.ts` (手動バッチ CLI) と crawl runner の自動採用フック (C1-b) が
 * 共有する実体。active KG (Discatier Core) の外部発話を話者アンカー
 * (`ext:<source>:<authorId>`) で集約し、`adoptPersonas` / `evaluateSpeakers` に渡す。
 *
 * 個人データ方針: 集約は内部のフル精度で行い、保存ペルソナの露出名は `論者#xxxxxx`
 * (`adoptPersonas` 側でマスク)。本モジュールは生 authorId を返さない。
 */

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { openAttributionStore } from "../crawler/sources/attribution-store.js";
import {
  adoptPersonas,
  evaluateSpeakers,
  type AdoptOptions,
  type SpeakerOpinions,
} from "./persona-adopt.js";

export interface AdoptRunOptions extends AdoptOptions {
  /** source 種別フィルタ (例 "youtube")。未指定なら全 source。 */
  sourceFilter?: string;
  /** 採用判定のみ実施し、プールへ書き込まない。 */
  dry?: boolean;
}

export interface AdoptRunSummary {
  /** 集約した外部話者数。 */
  speakers: number;
  /** 集約した外部発話数。 */
  utterances: number;
  /** 採用 (upsert) 件数。dry のときは採用候補数。 */
  adopted: number;
  /** 却下件数。 */
  rejected: number;
  /** 却下理由の内訳。 */
  rejectedByReason: Record<string, number>;
}

/**
 * active KG の外部発話を話者集約し、C1 採用条件で flow_persona へ upsert する。
 * crawl 完了後 (C1-b) / 手動バッチ (C1-a) の共通実体。
 */
export function runAdoptFromKg(opts: AdoptRunOptions = {}): AdoptRunSummary {
  const cfg = getConfig();
  const { sourceFilter, dry, ...adoptOpts } = opts;

  const core = createCore(resolveActiveKgPath(cfg));
  const attribution = openAttributionStore();
  try {
    const rows = core.client.raw
      .prepare(
        `SELECT id, speaker_id, raw_content FROM utterances
          WHERE workspace_id = ? AND speaker_id LIKE 'ext:%'`
      )
      .all(cfg.workspace) as Array<{ id: string; speaker_id: string; raw_content: string }>;

    // opinion-score (#125 (a)): 発話ごとの positive リアクション (いいね) 合算を weight に使う。
    const scoreRows = core.client.raw
      .prepare(
        `SELECT utterance_id, SUM(intensity) AS likes FROM reactions
          WHERE workspace_id = ? AND reaction_type = 'positive' GROUP BY utterance_id`
      )
      .all(cfg.workspace) as Array<{ utterance_id: string; likes: number }>;
    const likesById = new Map(scoreRows.map((r) => [r.utterance_id, r.likes ?? 0]));

    const bySpeaker = new Map<string, SpeakerOpinions>();
    for (const r of rows) {
      const attr = attribution.get(r.id);
      const source = attr?.source ?? r.speaker_id.split(":")[1];
      if (sourceFilter && source !== sourceFilter) continue;
      const sp = bySpeaker.get(r.speaker_id) ?? { speakerId: r.speaker_id, source, opinions: [] };
      // weight = 1 + いいね数 (0 いいねでも基準 1 で残す)。
      sp.opinions.push({
        text: r.raw_content,
        gameSlug: attr?.gameSlug ?? null,
        weight: 1 + (likesById.get(r.id) ?? 0),
      });
      bySpeaker.set(r.speaker_id, sp);
    }

    const speakers = [...bySpeaker.values()];

    if (dry) {
      const { candidates, rejected } = evaluateSpeakers(speakers, adoptOpts);
      return {
        speakers: speakers.length,
        utterances: rows.length,
        adopted: candidates.length,
        rejected: rejected.length,
        rejectedByReason: tallyReasons(rejected),
      };
    }

    const res = adoptPersonas(speakers, adoptOpts);
    return {
      speakers: speakers.length,
      utterances: rows.length,
      adopted: res.adopted.length,
      rejected: res.rejected.length,
      rejectedByReason: tallyReasons(res.rejected),
    };
  } finally {
    attribution.close();
    core.close?.();
  }
}

function tallyReasons(rejected: Array<{ reason: string }>): Record<string, number> {
  return rejected.reduce<Record<string, number>>((m, r) => {
    m[r.reason] = (m[r.reason] ?? 0) + 1;
    return m;
  }, {});
}
