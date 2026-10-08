/** Bounded UTF-8 delivery and safe errors shared by the two diagnostic adapters. */
import type { InteractionEditReplyOptions } from "discord.js";
import { DIAGNOSTIC_LIMITS as L, DiagnosticError, type DiagnosticFinding, type DesignGap } from "../design-diagnostic/contracts.js";
export function diagnosticReply(result: { logic_differences: DiagnosticFinding[]; design_gap: DesignGap[] }, filename: "mechanics-check.json" | "level-check.json"): InteractionEditReplyOptions {
  const json = JSON.stringify(result, null, 2);
  const bytes = Buffer.from(json, "utf8");
  if (bytes.length > L.resultBytes) throw new DiagnosticError("result_too_large", "Result attachment exceeds byte limit");
  const allowedMentions = { parse: [] as [] };
  if (json.length <= 1_850) return { content: `\`\`\`json\n${json}\n\`\`\``, allowedMentions };
  return { content: "logic_differences / design_gap", files: [{ attachment: bytes, name: filename }], allowedMentions };
}
export function diagnosticFailure(error: unknown): string {
  const code = error instanceof DiagnosticError ? error.code : "llm_failed";
  const reasons: Record<typeof code, string> = {
    invalid_input: "入力の型・長さ・ID・単位・確率を確認してください。",
    use_level_check: "playModel による場面・予測幅の診断には /level-check を使ってください。",
    llm_unavailable: "LLM が設定されていません。",
    llm_failed: "LLM の単発診断に失敗しました。",
    llm_timeout: "LLM の単発診断が時間内に完了しませんでした。",
    invalid_response: "LLM のJSON結果または根拠参照が不正です。",
    result_too_large: "診断結果が回答サイズの上限を超えました。",
  };
  return `診断失敗 (${code}): ${reasons[code]}`;
}
