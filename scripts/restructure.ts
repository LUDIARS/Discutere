/**
 * RESTRUCTURE パイプライン CLI ハーネス (spec §3)。
 *
 * 使用法:
 *   npx tsx scripts/restructure.ts <slug> [--mock] [--force-economy]
 *
 *   <slug>            data/games/<slug>.md に対応するゲーム識別子
 *   --mock            LLM 呼び出しをモックにする (決定論テスト用)
 *   --force-economy   step3 のキャッシュを無視して再生成
 *   --list            利用可能な slug を表示して終了
 *
 * 中間 JSON: data/restructure/<slug>/step{1..7}.json
 */

import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { createAnthropicAdapter, createMockAdapter } from "../src/restructure/llm-adapter.js";
import { runPipeline } from "../src/restructure/pipeline.js";
import { listAvailableSlugs } from "../src/restructure/step2-intake.js";
import { getConfig } from "../src/config.js";

const __dirname = fileURLToPath(new URL(".", import.meta.url));
const ROOT = join(__dirname, "..");

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const useMock = args.includes("--mock");
  const forceEconomy = args.includes("--force-economy");
  const listMode = args.includes("--list");
  const slug = args.find((a) => !a.startsWith("--"));

  const gamesDir = join(ROOT, "data", "games");
  const restructureDir = join(ROOT, "data", "restructure");
  const preferencesDir = join(ROOT, "data", "preferences");

  if (listMode) {
    const slugs = await listAvailableSlugs(gamesDir);
    console.log("Available slugs:");
    for (const s of slugs) console.log(`  ${s}`);
    return;
  }

  if (!slug) {
    console.error("Usage: npx tsx scripts/restructure.ts <slug> [--mock] [--force-economy]");
    console.error("       npx tsx scripts/restructure.ts --list");
    process.exit(1);
  }

  let llm;
  if (useMock) {
    console.log("[restructure] using mock LLM adapter");
    llm = createMockAdapter();
  } else {
    const config = getConfig();
    const apiKey = config.llm.anthropicApiKey;
    if (!apiKey) {
      console.error("[restructure] ANTHROPIC_API_KEY not set. Use --mock for testing.");
      process.exit(1);
    }
    llm = createAnthropicAdapter(apiKey);
  }

  try {
    const result = await runPipeline(slug, llm, {
      gamesDir,
      restructureDir,
      preferencesDir,
      forceRebuildEconomy: forceEconomy,
    });

    console.log("\n=== RESTRUCTURE SUMMARY ===");
    console.log(`Game:        ${result.step2.title} (${result.slug})`);
    console.log(`Mechanics:   ${result.step2.mechanics.length} (core=${result.step5.coreMechanics.length}, option=${result.step5.optionMechanics.length})`);
    console.log(`Economy:     resources=${result.step3.resources.length}, edges=${result.step3.edges.length}`);
    console.log(`Macro score: ${result.step4.macro.score.toFixed(2)} (${result.step4.macro.economyBalance})`);
    console.log(`Candidates:  ${result.step6.candidates.length}`);
    console.log(`Issues:      ${result.step7.issues.length} (iterations=${result.step7.iterationCount}, converged=${result.step7.converged})`);
    console.log(`Hypotheses:  ${result.step7.resolvedHypotheses.length}`);

    if (result.step7.resolvedHypotheses.length > 0) {
      console.log("\nProposed hypotheses:");
      for (const h of result.step7.resolvedHypotheses) {
        console.log(`  • ${h}`);
      }
    }

    if (result.step4.negativeGapCandidates.length > 0) {
      console.log("\nDesign gap candidates (negative evaluations):");
      for (const g of result.step4.negativeGapCandidates) {
        console.log(`  ⚠ ${g}`);
      }
    }

    console.log(`\nOutput: data/restructure/${slug}/step{1..7}.json`);
  } catch (err) {
    console.error("[restructure] pipeline failed:", (err as Error).message);
    process.exit(1);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
