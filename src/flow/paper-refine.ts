/**
 * ディスカッションペーパー本文の LLM リファイン (議論終了後)。
 *
 * 議論ループが終わった後に、base ブリーフ (議題 / 観点補足 / メカニクス) を
 * 議論の結果 (ラウンドまとめ / 止揚 / 結論) を踏まえて LLM で書き換える。
 *
 * 設計上の制約:
 *   - **議論ループの後**に走る。各ペルソナの system (buildPaperSystem) はループ中ずっと
 *     base のまま固定 = プロンプトキャッシュ安定。リファインはそれに影響しない。
 *   - セクション見出し (`# 議題` / `# 観点補足` / `# ゲームのメカニクス`) を保つよう指示し、
 *     出力が `# 議題` を欠く / 空なら **degrade** (= 元の base を使う)。議論は止めない。
 *   - 出力は base ブリーフのみ (`# 議論の経過` / `# 結論` は付けない。呼び出し側が付与する)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { RoundSummary } from "./discussion-paper.js";
import { withCostLog } from "./cost-logger.js";

const HEAD_THEME = "# 議題";

export interface RefinePaperBriefInput {
  /** 進行ログを含まない base ブリーフ md (議題 / 観点補足 / メカニクス)。 */
  baseBodyMd: string;
  theme: string;
  rounds: readonly RoundSummary[];
  conclusion?: string;
  llm: LLMClient;
  /** コストログ用。 */
  sessionId: string;
  flow?: string;
  /** リファインに使うモデル (未指定なら LLM 既定)。 */
  model?: string;
  warn?: (msg: string) => void;
}

/** ラウンドまとめ + 止揚を議論成果テキストにする。 */
function buildOutcomeText(rounds: readonly RoundSummary[], conclusion?: string): string {
  const parts: string[] = [];
  for (const r of rounds) {
    const lines = [`## ラウンド ${r.round}`, r.summary.trim() || "(まとめなし)"];
    if (r.aufhebung.length > 0) lines.push(`止揚: ${r.aufhebung.join(" / ")}`);
    parts.push(lines.join("\n"));
  }
  if (conclusion && conclusion.trim()) parts.push(`## 結論\n${conclusion.trim()}`);
  return parts.join("\n\n") || "(議論成果なし)";
}

/** リファイン指示プロンプトを組み立てる。 */
function buildRefinePrompt(input: RefinePaperBriefInput): string {
  return [
    "あなたはゲーム議論のディスカッションペーパー編集者です。",
    "以下の「現在のブリーフ」を、議論の成果を踏まえて改訂してください。",
    "",
    "# 制約",
    "- セクション見出し (# 議題 / # 観点補足 / # ゲームのメカニクス) を維持する。",
    "- 議題の主旨・タイトルは変えない。議論で深まった論点・観点を反映して具体化・整理する。",
    "- メカニクスは議論で重要と分かった項目を加筆/整理してよい (箇条書き `- **名前**: 説明` 形式を維持)。",
    "- 出力は markdown 本文のみ。「# 議論の経過」「# 結論」は付けない (別で付与する)。前置き・コードフェンスも不要。",
    "",
    "# 現在のブリーフ",
    input.baseBodyMd.trim() || "(空)",
    "",
    "# 議論の成果",
    buildOutcomeText(input.rounds, input.conclusion),
  ].join("\n");
}

/**
 * base ブリーフを議論の成果で改訂した md を返す。失敗 / 不正出力なら null (= 呼び出し側は元の base を使う)。
 */
export async function refinePaperBrief(input: RefinePaperBriefInput): Promise<string | null> {
  const { llm, sessionId, flow = "discussion", model, warn = console.warn } = input;
  const logged = withCostLog(llm, { flow, sessionId, location: "paper-refine" });

  const result = await logged.invoke({ prompt: buildRefinePrompt(input), model });
  if (!result.ok) {
    warn(`ペーパーリファイン失敗 (元のブリーフを使用): ${result.error}`);
    return null;
  }
  const text = result.text.trim();
  // 議題見出しを欠く / 空なら派生 (markdownToPaperDraft) が壊れるので degrade。
  if (!text || !text.includes(HEAD_THEME)) {
    warn("ペーパーリファイン出力が不正 (# 議題 欠落)。元のブリーフを使用");
    return null;
  }
  return text;
}
