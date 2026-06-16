/**
 * 議論の結論 (ConclusionDetail) → 単体 markdown レンダ (議論 md エクスポート)。
 *
 * 旧フロー (design_gap) / 新フロー (flow_conclusion) どちらの ConclusionDetail でも
 * 同じ形状なので 1 つの純関数で md 化できる。frontmatter (YAML) + 本文。
 * 個人アンカーは serializer 段でマスク済 (speaker は表示名 / 出所は source+URL のみ)。
 */

import matter from "gray-matter";

import type { ConclusionDetail } from "./conclusions.js";

/** ファイル名に使えない文字を除いた安全なスラグを作る。 */
export function safeSlug(title: string, fallback: string): string {
  const base = (title || "").replace(/[\\/:*?"<>|#%{}\s]+/g, "-").replace(/^-+|-+$/g, "");
  const slug = base.slice(0, 60);
  return slug || fallback;
}

/** 出所サフィックス (§6: source/sourceUrl は開示)。 無ければ空。 */
function sourceSuffix(o: { source: string | null; sourceUrl: string | null }): string {
  if (!o.source) return "";
  return o.sourceUrl ? ` ([${o.source}](${o.sourceUrl}))` : ` (${o.source})`;
}

/**
 * 結論詳細を 1 本の markdown 文字列にする。
 * 結論まとめ → 止揚ストック → 高評価意見 → 議論ログ の順。
 */
export function renderConclusionMarkdown(detail: ConclusionDetail): string {
  const fm: Record<string, unknown> = {
    type: "discussion-conclusion",
    kind: detail.kind,
    id: detail.gapId,
    session: detail.sessionId,
    title: detail.title,
    concluded: detail.conclusion !== null,
    utterances: detail.utteranceCount,
    aufhebungen: detail.aufhebungCount,
    updated_at: new Date(detail.updatedAt).toISOString(),
  };

  const lines: string[] = [];
  lines.push(`# ${detail.title}`);
  lines.push("");
  lines.push("## 結論");
  lines.push("");
  lines.push(detail.conclusion ?? "(まとめ未生成)");

  if (detail.aufhebungen.length > 0) {
    lines.push("");
    lines.push("## 止揚ストック");
    lines.push("");
    for (const a of detail.aufhebungen) {
      lines.push(`- ${a}`);
    }
  }

  if (detail.topOpinions.length > 0) {
    lines.push("");
    lines.push("## 高評価意見");
    lines.push("");
    for (const o of detail.topOpinions) {
      lines.push(`- **+${o.score} ${o.speaker}**: ${o.content}${sourceSuffix(o)}`);
    }
  }

  lines.push("");
  lines.push(`## 議論ログ (${detail.transcript.length} 発話)`);
  lines.push("");
  for (const u of detail.transcript) {
    lines.push(`- **${u.speaker}**: ${u.content}${sourceSuffix(u)}`);
  }

  return matter.stringify(`\n${lines.join("\n")}\n`, fm);
}
