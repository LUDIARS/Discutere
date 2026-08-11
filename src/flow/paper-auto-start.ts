/**
 * 外部で完成済みのディスカッションペーパーを、自動開始用 PaperDraft に正規化する。
 * DB / HTTP / LLM には依存せず、Web API や将来のバッチ入口から共用できる純関数とする。
 */

import { withDerivedStructure, type PaperDraft } from "./paper-review.js";
import type { FlowTag } from "./tags.js";

export class PaperAutoStartInputError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PaperAutoStartInputError";
  }
}

function firstLevelOneHeading(markdown: string): string {
  const match = /^#\s+(.+?)\s*$/m.exec(markdown);
  return match?.[1]?.trim() ?? "";
}

/** 完成済み Markdown を正本のまま保ち、議論エンジンへ渡せるドラフトにする。 */
export function paperDraftForAutoStart(args: {
  bodyMd: string;
  theme?: string;
  tags?: FlowTag[];
}): PaperDraft {
  const bodyMd = args.bodyMd.trim();
  if (!bodyMd) throw new PaperAutoStartInputError("ディスカッションペーパー本文は必須です");

  const fallbackTheme = args.theme?.trim() || firstLevelOneHeading(bodyMd);
  if (!fallbackTheme) {
    throw new PaperAutoStartInputError("テーマ、または Markdown の H1 見出しが必要です");
  }

  const draft = withDerivedStructure(bodyMd, {
    theme: fallbackTheme,
    tags: args.tags ?? [],
    supplement: "",
    mechanics: [],
  });
  if (!draft.theme.trim()) throw new PaperAutoStartInputError("ペーパーからテーマを解決できませんでした");
  return draft;
}
