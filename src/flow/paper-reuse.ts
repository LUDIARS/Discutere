/**
 * ディスカッションペーパーの使い回し (spec/feature/flow/paper-reuse.md)。
 *
 * 新しい議論を始めるとき、既存ペーパーを議論番号 (discussion_no) で指定すると、
 *   - メカニクス (mechanics_json) と ゲーム内容 (本文 md の mechanicsContext)
 *   - 保存した声 (paper-voices.ts)
 * を引き継ぐ。声を引き継げたときは集め直さない (ユーザーの声の収集・情報ゲートを省く)。
 * 指定は Discord の starter / Web の本文中の「ペーパー#12」(「paper#12」「ペーパー 12」も可) か、
 * Web の reusePaperNo 欄。
 */

import { getFlowDb } from "./db/connection.js";
import type { ContextVoice } from "./discussion-paper.js";
import type { MechanicSummary } from "./investigate.js";
import { paperFixedFieldsFromMarkdown, stripProgress } from "./paper-markdown.js";
import { loadPaperVoices } from "./paper-voices.js";

const REUSE_RE = /(?:ペーパー|paper)\s*[#＃]?\s*([0-9０-９]+)/i;

/** 本文から使い回すペーパー番号を読む (無ければ null)。 */
export function parseReusePaperNo(text: string | undefined): number | null {
  if (!text) return null;
  const m = text.match(REUSE_RE);
  if (!m) return null;
  const n = Number(m[1].replace(/[０-９]/g, (d) => String.fromCharCode(d.charCodeAt(0) - 0xfee0)));
  return Number.isInteger(n) && n > 0 ? n : null;
}

export interface ReusablePaper {
  discussionNo: number;
  sessionId: string;
  theme: string;
  mechanics: MechanicSummary[];
  /** ゲーム内容 (本文の固定欄)。無ければ ""。 */
  mechanicsContext: string;
  voices: ContextVoice[];
}

function parseMechanics(json: string | null): MechanicSummary[] {
  try {
    const v = JSON.parse(json ?? "[]");
    return Array.isArray(v) ? (v as MechanicSummary[]) : [];
  } catch {
    return [];
  }
}

/** 議論番号のペーパーを引く (無ければ null)。 */
export function loadReusablePaper(discussionNo: number): ReusablePaper | null {
  const row = getFlowDb()
    .prepare(
      `SELECT session_id, theme, tags_json, mechanics_json, supplement, body_md
         FROM discussion_paper WHERE discussion_no = ? LIMIT 1`
    )
    .get(discussionNo) as
    | { session_id: string; theme: string; tags_json: string; mechanics_json: string; supplement: string; body_md: string | null }
    | undefined;
  if (!row) return null;
  const mechanics = parseMechanics(row.mechanics_json);
  const mechanicsContext = row.body_md
    ? paperFixedFieldsFromMarkdown(stripProgress(row.body_md), {
        theme: row.theme,
        tags: [],
        supplement: row.supplement,
        mechanics,
      }).mechanicsContext
    : "";
  return {
    discussionNo,
    sessionId: row.session_id,
    theme: row.theme,
    mechanics,
    mechanicsContext,
    voices: loadPaperVoices(row.session_id),
  };
}

/** 引き継ぐゲーム内容 (元ペーパーの内容 + 今回の資料。重複は除く)。 */
export function mergeReusedContext(reused: ReusablePaper | null, current: string | undefined): string | undefined {
  const parts = [reused?.mechanicsContext ?? "", current ?? ""].map((s) => s.trim()).filter(Boolean);
  const unique = [...new Set(parts)];
  return unique.length > 0 ? unique.join("\n\n") : undefined;
}

/** 使い回しを知らせる一言。 */
export function describeReuse(no: number, reused: ReusablePaper | null): string {
  if (!reused) return `♻️ ペーパー #${no} が見つからないため、通常どおり材料を集めます。`;
  const voices =
    reused.voices.length > 0
      ? `声 ${reused.voices.length} 件 (集め直しません)`
      : "声 (保存が無いため通常どおり集めます)";
  return `♻️ ペーパー #${no}「${reused.theme}」のメカニクス ${reused.mechanics.length} 件・ゲーム内容・${voices}を使い回します。`;
}
