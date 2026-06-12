/**
 * RESTRUCTURE パイプライン統括。
 *
 * step1〜7 を順に実行し、各ステップの中間 JSON を
 * data/restructure/<slug>/step{1..7}.json に保存する。
 *
 * spec/RESTRUCTURE.md §3 テストハーネス仕様に準拠。
 */

import { readFile, writeFile, mkdir } from "node:fs/promises";
import { join } from "node:path";
import type { LlmAdapter } from "./llm-adapter.js";
import type { RestructurePipelineOutput } from "./types.js";
import { buildPreferences, savePreferences } from "./step1-preferences.js";
import { intakeGame } from "./step2-intake.js";
import { buildEconomy } from "./step3-economy.js";
import { evaluateGame } from "./step4-evaluate.js";
import { decomposeMechanics } from "./step5-decompose.js";
import { combineMechanics } from "./step6-combine.js";
import { fixIncoherence } from "./step7-fix.js";

export interface PipelineOptions {
  /** data/games/ へのパス */
  gamesDir: string;
  /** data/restructure/ へのパス */
  restructureDir: string;
  /** data/preferences/ へのパス */
  preferencesDir: string;
  /** step3 economy グラフを再生成する場合 true */
  forceRebuildEconomy?: boolean;
}

async function saveStep<T>(dir: string, slug: string, step: number, data: T): Promise<void> {
  const stepDir = join(dir, slug);
  await mkdir(stepDir, { recursive: true });
  await writeFile(join(stepDir, `step${step}.json`), JSON.stringify(data, null, 2), "utf-8");
}

export async function runPipeline(
  slug: string,
  llm: LlmAdapter,
  opts: PipelineOptions,
): Promise<RestructurePipelineOutput> {
  console.log(`[restructure] start: ${slug}`);

  // ① 嗜好プロファイル
  console.log(`[restructure] step1: building preferences...`);
  const step1 = await buildPreferences(opts.gamesDir);
  await savePreferences(opts.preferencesDir, step1);
  await saveStep(opts.restructureDir, slug, 1, step1);
  console.log(`[restructure] step1: ${step1.profiles.length} profiles`);

  // ② 取り込み
  console.log(`[restructure] step2: loading game "${slug}"...`);
  const step2 = await intakeGame(opts.gamesDir, slug);
  await saveStep(opts.restructureDir, slug, 2, step2);
  console.log(`[restructure] step2: ${step2.mechanics.length} mechanics (source=${step2.source})`);

  // ③ 内部経済グラフ
  console.log(`[restructure] step3: building economy graph...`);
  const step3 = await buildEconomy(step2, opts.restructureDir, llm, { force: opts.forceRebuildEconomy });
  await saveStep(opts.restructureDir, slug, 3, step3);
  console.log(`[restructure] step3: ${step3.resources.length} resources, ${step3.edges.length} edges (cached=${step3.cached})`);

  // ④ 評価
  console.log(`[restructure] step4: evaluating mechanics...`);
  const step4 = await evaluateGame(step2, step3, step1.profiles, llm);
  await saveStep(opts.restructureDir, slug, 4, step4);
  console.log(`[restructure] step4: micro=${step4.micro.length} evals, macro.score=${step4.macro.score.toFixed(2)}, negatives=${step4.negativeGapCandidates.length}`);

  // ⑤ コア/オプション分解
  console.log(`[restructure] step5: decomposing core/option...`);
  const step5 = await decomposeMechanics(step2, step3, llm);
  await saveStep(opts.restructureDir, slug, 5, step5);
  console.log(`[restructure] step5: core=${step5.coreMechanics.length}, option=${step5.optionMechanics.length}`);

  // ⑥ 他ゲームのメカニクス結合
  console.log(`[restructure] step6: finding combination candidates...`);
  const step6 = await combineMechanics(step2, step4, step5, step1.profiles, opts.gamesDir, opts.restructureDir);
  await saveStep(opts.restructureDir, slug, 6, step6);
  console.log(`[restructure] step6: ${step6.candidates.length} candidates`);

  // ⑦ 破綻検出・修正
  console.log(`[restructure] step7: detecting and fixing incoherence...`);
  const existingNames = new Set(step2.mechanics.map((m) => m.name.toLowerCase()));
  const step7 = await fixIncoherence(step6, step3, existingNames, llm);
  await saveStep(opts.restructureDir, slug, 7, step7);
  console.log(`[restructure] step7: issues=${step7.issues.length}, hypotheses=${step7.resolvedHypotheses.length}, converged=${step7.converged}`);

  const output: RestructurePipelineOutput = { slug, step1, step2, step3, step4, step5, step6, step7 };
  await saveStep(opts.restructureDir, slug, 0, output);

  console.log(`[restructure] done: ${slug}`);
  return output;
}

/** 保存済みの特定ステップ JSON を読み込む */
export async function loadStepOutput<T>(restructureDir: string, slug: string, step: number): Promise<T | null> {
  try {
    const raw = await readFile(join(restructureDir, slug, `step${step}.json`), "utf-8");
    return JSON.parse(raw) as T;
  } catch {
    return null;
  }
}
