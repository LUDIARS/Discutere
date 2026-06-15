/**
 * ペルソナ合成 CLI (F)。
 *
 *   npm run persona:synthesize -- --parents "ローグ好き太郎,ソシャゲ花子"
 *   npm run persona:synthesize -- --parents a,b --label "ローグ×ソシャゲ"
 *
 * 親 (id/name, カンマ区切り 2 体以上) を融合した新ペルソナをプール (flow_persona) に保存する。
 * LLM はサブスク OAuth (Claude Code 認証) → claude-p フォールバック。
 */

import { getConfig } from "../src/config.js";
import { AnthropicSdkClient } from "../src/persona-engine/llm/anthropic.js";
import { ClaudeCliClient } from "../src/persona-engine/llm/claude-cli.js";
import { readClaudeCodeToken } from "../src/persona-engine/llm/claude-code-auth.js";
import { FallbackLlm } from "../src/flow/llm-fallback.js";
import { synthesizePersona } from "../src/flow/persona-synthesize.js";

function arg(name: string): string | undefined {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : undefined;
}

async function main(): Promise<void> {
  const parentsRaw = arg("parents");
  if (!parentsRaw) {
    console.error('使い方: npm run persona:synthesize -- --parents "親1,親2" [--label ラベル]');
    process.exit(1);
  }
  const parentRefs = parentsRaw.split(/[,、]/).map((s) => s.trim()).filter(Boolean);
  const label = arg("label");
  const cfg = getConfig();

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

  console.log(`合成: [${parentRefs.join(", ")}] → 新ペルソナ`);
  const persona = await synthesizePersona({ parentRefs, llm, label });
  console.log("✅ 保存しました:");
  console.log(`  id    : ${persona.id}`);
  console.log(`  name  : ${persona.name}`);
  console.log(`  口調  : ${persona.speechStyle}`);
  console.log(`  特徴  : ${persona.traits.join(" / ")}`);
  console.log(`  親    : ${persona.parentIds.join(", ")}`);
}

main().catch((e) => {
  console.error(`NG: ${(e as Error).message}`);
  process.exit(1);
});
