/**
 * 実在ユーザ採用 CLI (C1-a)。
 *
 *   npm run persona:adopt -- [--source steam] [--min-opinions 10] [--dry]
 *
 * crawl 完了後に KG (Discatier Core) の外部発話を話者 (`ext:<source>:<authorId>`) で集約し、
 * 採用条件 (横断 ≥N / 全ネガ全ポジ除外 / ゲーム間 gap) を満たす人物を flow_persona に upsert する。
 * 露出名は `論者#xxxxxx`、affect は本人意見の平均、口調なし。
 */

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { openAttributionStore } from "../src/crawler/sources/attribution-store.js";
import { adoptPersonas, type SpeakerOpinions } from "../src/flow/persona-adopt.js";

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
function has(name: string): boolean {
  return process.argv.includes(`--${name}`);
}

async function main(): Promise<void> {
  const cfg = getConfig();
  const sourceFilter = arg("source");
  const minOpinions = arg("min-opinions") ? Number(arg("min-opinions")) : 10;
  const dry = has("dry");

  const core = createCore(resolveActiveKgPath(cfg));
  const attribution = openAttributionStore();
  try {
    // 外部発話を speaker_id で集約。
    const rows = core.client.raw
      .prepare(
        `SELECT id, speaker_id, raw_content FROM utterances
          WHERE workspace_id = ? AND speaker_id LIKE 'ext:%'`
      )
      .all(cfg.workspace) as Array<{ id: string; speaker_id: string; raw_content: string }>;

    const bySpeaker = new Map<string, SpeakerOpinions>();
    for (const r of rows) {
      const attr = attribution.get(r.id);
      const source = attr?.source ?? r.speaker_id.split(":")[1];
      if (sourceFilter && source !== sourceFilter) continue;
      const sp = bySpeaker.get(r.speaker_id) ?? { speakerId: r.speaker_id, source, opinions: [] };
      sp.opinions.push({ text: r.raw_content, gameSlug: attr?.gameSlug ?? null });
      bySpeaker.set(r.speaker_id, sp);
    }

    const speakers = [...bySpeaker.values()];
    console.log(`外部話者 ${speakers.length} 名を集約 (発話 ${rows.length} 件, source=${sourceFilter ?? "all"})`);

    if (dry) {
      // 採用判定のみ (書込なし)。
      const { evaluateSpeakers } = await import("../src/flow/persona-adopt.js");
      const { candidates, rejected } = evaluateSpeakers(speakers, { minOpinions });
      console.log(`[dry] 採用候補 ${candidates.length} / 却下 ${rejected.length}`);
      const byReason = rejected.reduce<Record<string, number>>((m, r) => ((m[r.reason] = (m[r.reason] ?? 0) + 1), m), {});
      console.log("[dry] 却下内訳:", byReason);
      return;
    }

    const res = adoptPersonas(speakers, { minOpinions });
    console.log(`✅ 採用 ${res.adopted.length} 名 / 却下 ${res.rejected.length}`);
    for (const p of res.adopted.slice(0, 20)) {
      console.log(`  ${p.name} (src=${p.learningSource ?? "-"}, typicality=${p.typicality})`);
    }
    const byReason = res.rejected.reduce<Record<string, number>>((m, r) => ((m[r.reason] = (m[r.reason] ?? 0) + 1), m), {});
    console.log("却下内訳:", byReason);
  } finally {
    attribution.close();
    core.close?.();
  }
}

main().catch((e) => {
  console.error(`NG: ${(e as Error).message}`);
  process.exit(1);
});
