/**
 * 外部取り込みデータの要約器 (id=67)。
 *
 * 長文 source (website 記事) を LLM で要約し、 ExternalUtterance.summary に詰める。
 * 議論コンテキスト参照はこの要約を使ってトークンを節約し、 raw 全文 (content) は
 * importer 側で raw-store に退避する 2 層化。
 *
 * `summarizeItems` は items を mutate して `.summary` を埋める (要約できたものだけ)。
 */

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { ExternalUtterance } from "./types.js";

export interface Summarizer {
  /** text を要約。 失敗時は null (呼び出し側は raw を使う)。 */
  summarize(text: string): Promise<string | null>;
}

export interface LlmSummarizerOptions {
  /** モデル上書き */
  model?: string;
  /** 目安の要約字数 (既定 400) */
  targetChars?: number;
  /** LLM に渡す入力の上限字数 (既定 12000) */
  inputCap?: number;
}

/** LLMClient を要約器に変換する。 */
export function createLlmSummarizer(llm: LLMClient, opts: LlmSummarizerOptions = {}): Summarizer {
  const targetChars = opts.targetChars ?? 400;
  const inputCap = opts.inputCap ?? 12000;
  const system =
    "あなたはゲーム議論データの要約器です。 与えられた記事 (レビュー / 考察) を、" +
    " 後段の議論の素として使える形に日本語で簡潔に要約してください。" +
    ` 主張・評価・具体例・賛否の要点だけを残し、 約 ${targetChars} 字以内。` +
    " 前置き・見出し・箇条書き記号は付けず、 地の文で書く。";
  return {
    async summarize(text: string): Promise<string | null> {
      const trimmed = text.trim();
      if (!trimmed) return null;
      const res = await llm.invoke({ prompt: trimmed.slice(0, inputCap), system, model: opts.model });
      if (!res.ok) return null;
      const s = res.text.trim();
      if (!s) return null;
      // 暴走防止 (目安字数の 2 倍で安全側にカット)。
      return s.length > targetChars * 2 ? `${s.slice(0, targetChars * 2 - 1)}…` : s;
    },
  };
}

export interface SummarizeOptions {
  /** これ未満の content は要約しない (短文 source = comment はそのまま)。 既定 600 */
  minChars?: number;
  /** 1 件要約するごとの進捗 (累計件数) */
  onItem?: (done: number) => void;
}

/**
 * items を走査して長文のものに要約を付与する (mutate)。 付与した件数を返す。
 * 既に summary がある / content が短い / 要約失敗 のものは raw のまま。
 */
export async function summarizeItems(
  items: ExternalUtterance[],
  summarizer: Summarizer,
  opts: SummarizeOptions = {}
): Promise<number> {
  const minChars = opts.minChars ?? 600;
  let done = 0;
  for (const it of items) {
    if (it.summary && it.summary.trim()) continue;
    if ((it.content?.length ?? 0) < minChars) continue;
    const s = await summarizer.summarize(it.content);
    if (s) {
      it.summary = s;
      done += 1;
      opts.onItem?.(done);
    }
  }
  return done;
}
