/**
 * 結論一覧キャッシュ (conclusion_cache) のバックフィル + 材料件数 (materialCount) 算出。
 *
 *   npm run build:conclusion-cache
 *
 * - 既存の全結論 (旧 design_gaps + 新 flow_conclusion) を conclusion_cache へ upsert する
 *   (discussionVolume = 発話数。収束 write-through と同じ値)。
 * - 各話題の materialCount = active KG に取り込まれた外部クロール材料 (speaker_id='ext:%') の
 *   うち、話題のキーワードを本文に含む件数 (上限なしの直接 COUNT = 重い KG スキャン)。
 *   これは「別途計算してキャッシュ」する重い部分で、一覧閲覧経路からは外す。
 *
 * 議論収束ごとの discussionVolume 更新は flow/conclusion.ts が write-through する。
 * 本スクリプトは materialCount 充足 + 過去分バックフィルのためにバッチで回す運用。
 */

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { getFlowDb } from "../src/flow/db/connection.js";
import { listConclusions } from "../src/visualize/conclusions.js";
import { listFlowConclusions } from "../src/visualize/flow-conclusions.js";
import {
  upsertConclusionCache,
  recomputeMaterialCounts,
} from "../src/visualize/conclusion-cache.js";
import type { ConclusionSummary } from "../src/visualize/conclusions.js";
import { topicTermsFromTitle } from "../src/visualize/topic-terms.js";
import { buildAliasGroups, expandAliases } from "../src/discatier-engine-adapter/game-aliases.js";

type Core = ReturnType<typeof createCore>;

/** 話題に紐づく外部クロール材料の件数を KG から直接 COUNT する (上限なし)。 */
function countTopicMaterial(
  core: Core,
  workspaceId: string,
  title: string,
  aliasGroups: string[][]
): number {
  // 話題語をゲーム名の別名 (略称⇄正式名) で拡張して取りこぼしを減らす (#301)。
  const terms = expandAliases(topicTermsFromTitle(title), aliasGroups);
  if (terms.length === 0) return 0;
  // term ごとの LIKE を OR 連結し、重複なく 1 回でカウントする。
  const where = terms.map(() => "lower(raw_content) LIKE ?").join(" OR ");
  const params = terms.map((t) => `%${t.toLowerCase()}%`);
  const row = core.client.raw
    .prepare(
      `SELECT COUNT(*) AS n
         FROM utterances
        WHERE workspace_id = ? AND speaker_id LIKE 'ext:%' AND (${where})`
    )
    .get(workspaceId, ...params) as { n: number };
  return row.n;
}

function main(): void {
  const config = getConfig();
  const flowDb = getFlowDb();
  const core = createCore(resolveActiveKgPath(config));
  const onlyMaterial = process.argv.includes("--material-only");
  // ゲーム名の別名グループは KG の games タイトルから 1 回だけ構築 (略称⇄正式名・和名⇄英名)。
  const titles = (
    core.client.raw
      .prepare("SELECT title FROM games WHERE workspace_id = ?")
      .all(config.workspace) as Array<{ title: string }>
  ).map((g) => g.title);
  const aliasGroups = buildAliasGroups(titles);
  try {
    if (!onlyMaterial) {
      // 1) 全結論を upsert (発話数 = discussionVolume)。materialCount は温存 (-1 渡し)。
      const gap = listConclusions(core, config.workspace, 100000);
      const flow = listFlowConclusions(flowDb, 100000);
      const all: ConclusionSummary[] = [...gap, ...flow];
      const run = flowDb.transaction((list: ConclusionSummary[]) => {
        for (const s of list) {
          upsertConclusionCache(flowDb, s, { discussionVolume: s.utteranceCount });
        }
      });
      run(all);
      console.log(`upsert: gap=${gap.length} flow=${flow.length} total=${all.length}`);
    }

    // 2) materialCount を別途算出して書き戻す (重い KG スキャン)。
    const updated = recomputeMaterialCounts(flowDb, (s) =>
      countTopicMaterial(core, config.workspace, s.title, aliasGroups)
    );
    console.log(`materialCount recomputed: ${updated} rows`);
  } finally {
    core.close();
  }
}

main();
