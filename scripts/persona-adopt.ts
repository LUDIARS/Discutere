/**
 * 実在ユーザ採用 CLI (C1-a)。
 *
 *   npm run persona:adopt -- [--source steam] [--min-opinions 10] [--dry]
 *
 * crawl 完了後に KG (Discatier Core) の外部発話を話者 (`ext:<source>:<authorId>`) で集約し、
 * 採用条件 (横断 ≥N / 全ネガ全ポジ除外 / ゲーム間 gap) を満たす人物を flow_persona に upsert する。
 * 露出名は `論者#xxxxxx`、affect は本人意見の平均、口調なし。
 * 集約 + 採用の実体は `src/flow/persona-adopt-runner.ts` (crawl 自動採用フック C1-b と共有)。
 */

import { runAdoptFromKg } from "../src/flow/persona-adopt-runner.js";

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}
function has(name: string): boolean {
  return process.argv.includes(`--${name}`);
}

async function main(): Promise<void> {
  const sourceFilter = arg("source");
  const minOpinions = arg("min-opinions") ? Number(arg("min-opinions")) : 10;
  const dry = has("dry");

  const summary = runAdoptFromKg({ sourceFilter, minOpinions, dry });
  console.log(
    `外部話者 ${summary.speakers} 名を集約 (発話 ${summary.utterances} 件, source=${sourceFilter ?? "all"})`
  );
  if (dry) {
    console.log(`[dry] 採用候補 ${summary.adopted} / 却下 ${summary.rejected}`);
    console.log("[dry] 却下内訳:", summary.rejectedByReason);
    return;
  }
  console.log(`✅ 採用 ${summary.adopted} 名 / 却下 ${summary.rejected}`);
  console.log("却下内訳:", summary.rejectedByReason);
}

main().catch((e) => {
  console.error(`NG: ${(e as Error).message}`);
  process.exit(1);
});
