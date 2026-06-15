/**
 * ペルソナ生成エンジン CLI (C2-a)。
 *
 *   npm run persona:survey -- --count 30 [--games-per 8]
 *
 * 合成個体を count 体生成し、data/games/*.md のゲームについて行動基準で「プレイしたか」を
 * 判定 → プレイ済みのみ LLM 感想 → affect 平均でプール投入 (origin="generated")。
 * 母数推定は `npm run persona:populations` (C2-b)。LLM=SDK OAuth→claude-p。
 */

import fs from "node:fs";
import path from "node:path";
import { getConfig } from "../src/config.js";
import { AnthropicSdkClient } from "../src/persona-engine/llm/anthropic.js";
import { ClaudeCliClient } from "../src/persona-engine/llm/claude-cli.js";
import { readClaudeCodeToken } from "../src/persona-engine/llm/claude-code-auth.js";
import { FallbackLlm } from "../src/flow/llm-fallback.js";
import { generateSyntheticPersonas, type SurveyGame } from "../src/flow/persona-survey.js";

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

function loadGames(): SurveyGame[] {
  const dir = path.resolve("data/games");
  if (!fs.existsSync(dir)) return [];
  return fs
    .readdirSync(dir)
    .filter((f) => f.endsWith(".md"))
    .map((f) => {
      const slug = f.replace(/\.md$/, "");
      return { slug, title: slug };
    });
}

async function main(): Promise<void> {
  const cfg = getConfig();
  const count = Number(arg("count") ?? "20");
  const gamesPerIndividual = arg("games-per") ? Number(arg("games-per")) : undefined;
  const games = loadGames();
  if (games.length === 0) {
    console.error("data/games/*.md が見つかりません。ゲーム母集団が空です。");
    process.exit(1);
  }

  const llm = new FallbackLlm(
    new AnthropicSdkClient({
      getAuthToken: () => readClaudeCodeToken(),
      apiKey: cfg.llm.anthropicApiKey || undefined,
      defaultModel: cfg.llm.model || undefined,
    }),
    new ClaudeCliClient({
      defaultTimeoutMs: cfg.llm.claudeCliTimeoutMs,
      defaultModel: cfg.llm.model || undefined,
      gitBashPath: cfg.workerPool.gitBashPath ?? cfg.llm.gitBashPath,
    })
  );

  console.log(`合成 ${count} 体 (ゲーム母集団 ${games.length} 本) を生成中…`);
  const res = await generateSyntheticPersonas({ count, games, llm, gamesPerIndividual });
  const totalPlayed = res.breakdown.reduce((s, b) => s + b.played, 0);
  const totalUnplayed = res.breakdown.reduce((s, b) => s + b.unplayed, 0);
  console.log(`✅ 保存 ${res.personas.length} 体 (プレイ感想 ${totalPlayed} / 未プレイ ${totalUnplayed})`);
  console.log("  母数推定は: npm run persona:populations");
}

main().catch((e) => {
  console.error(`NG: ${(e as Error).message}`);
  process.exit(1);
});
