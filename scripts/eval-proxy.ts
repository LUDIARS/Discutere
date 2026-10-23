/**
 * 代理の本人らしさを測る (spec/feature/flow/proxy-debate.md §5、統合レビュー §2.3)。
 *
 *   npm run eval:proxy -- --genre <id> [--heldout <heldout.jsonl>] [--session <id> ...] [--write]
 *
 * 取り込み済みの人物パック (そのジャンル・失効していないもの) を 1 人ずつ評価する。
 *   - 推薦予測 (必須): パックの heldOut で測る
 *   - 判別: --heldout (proxy:form --heldout-out の出力) があるとき、同じジャンルの次の人物を相手に測る
 *   - 根拠付き率 / 誤帰属率: --session で代理議論のセッションを指定したとき
 * --write で合否を flow_proxy_profile.eval_status に書き戻す (代理に出せるかがこれで決まる)。
 * 実 LLM を呼ぶ。LLM の設定は llm.backend (local / claude-cli / anthropic) に従う。
 */

import { readFileSync } from "node:fs";
import path from "node:path";

import { getConfig } from "../src/config.js";
import type { LLMClient } from "../src/persona-engine/llm/client.js";
import { AnthropicSdkClient } from "../src/persona-engine/llm/anthropic.js";
import { ClaudeCliClient } from "../src/persona-engine/llm/claude-cli.js";
import { LocalOpenAiClient } from "../src/persona-engine/llm/local-openai.js";
import { listProxyProfiles, setProxyEvalStatus } from "../src/flow/proxy/store.js";
import { evaluateProxy } from "../src/flow/proxy/eval/run.js";

function valueOf(flag: string): string | undefined {
  const index = process.argv.indexOf(flag);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  return value && !value.startsWith("--") ? value : undefined;
}

function valuesOf(flag: string): string[] {
  const out: string[] = [];
  process.argv.forEach((a, i) => {
    const v = process.argv[i + 1];
    if (a === flag && v && !v.startsWith("--")) out.push(v);
  });
  return out;
}

/** 評価専用スクリプトなので LLM が無ければ止める (黙って測らずに合格にしない)。 */
function buildLlm(): LLMClient {
  const config = getConfig();
  if (config.llm.backend === "local") {
    return new LocalOpenAiClient({
      baseUrl: config.llm.local.baseUrl,
      defaultModel: config.llm.local.model,
      apiKey: config.llm.local.apiKey,
      defaultTimeoutMs: config.llm.local.timeoutMs,
    });
  }
  if (config.llm.backend === "claude-cli") {
    return new ClaudeCliClient({
      defaultTimeoutMs: config.llm.claudeCliTimeoutMs,
      defaultModel: config.llm.model,
      gitBashPath: config.llm.gitBashPath,
    });
  }
  if (config.llm.anthropicApiKey) {
    return new AnthropicSdkClient({ apiKey: config.llm.anthropicApiKey, defaultModel: config.llm.model });
  }
  throw new Error("eval:proxy には LLM が必要です (llm.backend=local / claude-cli か ANTHROPIC_API_KEY)");
}

interface HeldOutLine {
  speakerKey: string;
  genre: string;
  text: string;
}

function readHeldOut(file: string | undefined): HeldOutLine[] {
  if (!file) return [];
  return readFileSync(path.resolve(file), "utf8")
    .split(/\r?\n/)
    .filter((l) => l.trim())
    .map((l) => JSON.parse(l) as HeldOutLine);
}

const genre = valueOf("--genre");
if (!genre) {
  console.error("usage: npm run eval:proxy -- --genre <id> [--heldout <file>] [--session <id> ...] [--write]");
  process.exit(2);
}
const write = process.argv.includes("--write");
const heldOut = readHeldOut(valueOf("--heldout"));
const sessionIds = valuesOf("--session");
const config = getConfig();
const llm = buildLlm();
const model = config.flow.proxy.stanceModel || config.flow.dialectic.judgeModel || undefined;

const rows = listProxyProfiles({ genre });
if (rows.length === 0) {
  console.error(`ジャンル ${genre} の人物パックがありません (npm run proxy:import で取り込む)`);
  process.exit(1);
}

for (let i = 0; i < rows.length; i++) {
  const row = rows[i];
  const partner = rows.length > 1 ? rows[(i + 1) % rows.length] : undefined;
  const texts = heldOut.filter((h) => h.speakerKey === row.speakerKey && h.genre === genre).map((h) => h.text);
  const report = await evaluateProxy({
    row,
    partner,
    heldOutTexts: texts,
    sessionIds,
    budgetTokens: config.flow.proxy.budgetTokens,
    llm,
    model,
  });
  const m = report.metrics;
  const fmt = (v: number | null) => (v === null ? "未測定" : v.toFixed(2));
  console.log(
    `${row.label} (${genre}): ${report.verdict.status} — 推薦予測 ${fmt(m.recommendation.accuracy)} ` +
      `(多数派 ${fmt(m.recommendation.majorityAccuracy)}, ${m.recommendation.total} 本, 分からない ${m.recommendation.abstained}) / ` +
      `判別 ${fmt(m.discrimination)} / 根拠付き ${fmt(m.groundedRate)} / 誤帰属 ${fmt(m.misattributionRate)}`
  );
  for (const reason of report.verdict.reasons) console.log(`  - ${reason}`);
  if (write) setProxyEvalStatus(row.speakerKey, genre, report.verdict.status, JSON.stringify(report), Date.now());
}
