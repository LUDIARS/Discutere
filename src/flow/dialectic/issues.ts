/**
 * 論点分解 [0] — テーマ + ペーパーから「賛否が本気で割れる論点」を分解する。
 *
 * dialectic エンジン (spec/feature/flow/dialectic.md) の入口 [0] と、議論適性ゲート
 * (09-paper-gate-debatability の争点の存在チェック) が **同一関数** を共用する。
 * PR-D では議論適性ゲート + ペーパー `# 論点` 節の前倒しに使い、PR-B の dialectic
 * エンジンは承認済み論点があれば再分解せずこれを [0] の結果として採用する。
 *
 * DB 非依存・LLM 注入 (コストログのラップは呼び出し側)。失敗は握り潰さず throw する
 * (呼び出し側 = debatability ゲートが degrade を明示して議論を止めない)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import { extractJsonArray } from "../mechanic-extract.js";

/** 論点分解の目標範囲 (dialectic.md §8: 3〜5 個クランプ)。 */
export const ISSUES_MIN = 3;
export const ISSUES_MAX = 5;

export interface DecomposeIssuesArgs {
  theme: string;
  /** ペーパー本文 md (議題ブリーフ)。論点分解の文脈に使う。 */
  paperMd?: string;
  llm: LLMClient;
  model?: string;
  /** 上限 (既定 ISSUES_MAX=5)。超過分は切り捨てる。 */
  maxIssues?: number;
  warn?: (msg: string) => void;
}

/**
 * 賛否が本気で割れる論点を 0〜maxIssues 個に分解する。
 * 争点が本当に無ければ空配列 (それ自体が議論適性ゲートの判定材料)。
 * LLM 呼び出し失敗 / JSON 解析失敗は throw (呼び出し側が degrade を明示する)。
 */
export async function decomposeIssues(args: DecomposeIssuesArgs): Promise<string[]> {
  const { theme, paperMd, llm, model, maxIssues = ISSUES_MAX } = args;

  const system =
    "あなたは議論設計者です。 与えられた議題とディスカッションペーパーから、" +
    "「賛成側と反対側が本気で対立できる論点 (争点)」だけを分解します。" +
    " 単なる確認事項や、誰も反対しない自明な項目は含めません。 返答は JSON 配列のみ。";
  const prompt =
    `# 議題\n${theme}\n\n` +
    (paperMd && paperMd.trim() ? `# ディスカッションペーパー\n${paperMd.trim()}\n\n` : "") +
    `賛否が本気で割れる論点を ${ISSUES_MIN}〜${Math.max(ISSUES_MIN, maxIssues)} 個を目安に、` +
    "各 1 文の疑問文で挙げてください。 本当に争点が無い場合は空配列を返して構いません。\n" +
    '次の JSON 配列だけを返してください (前後に説明やコードフェンスを付けない):\n["論点1", "論点2", ...]';

  const res = await llm.invoke({ system, prompt, model });
  if (!res.ok) {
    throw new Error(`論点分解 LLM エラー: ${res.error}`);
  }
  const arr = extractJsonArray(res.text);
  if (!arr) {
    throw new Error("論点分解の JSON 解析に失敗");
  }
  const issues = arr
    .filter((v): v is string => typeof v === "string")
    .map((s) => s.trim())
    .filter(Boolean);
  return issues.slice(0, Math.max(1, maxIssues));
}
