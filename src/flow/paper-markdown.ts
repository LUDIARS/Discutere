/**
 * ディスカッションペーパー本文の **markdown 正本化** (ハイブリッド源泉モデル)。
 *
 * ペーパーは構造化フィールド (theme / tags / supplement / mechanics) を持つが、
 * 議論ブリーフの **本文 (議題 / 観点補足 / メカニクス) は markdown を正本** とする:
 *   - 各 LLM (ペルソナ) には buildPaperSystem がこの md をそのまま system に載せる。
 *   - Web の Notion 風エディタはこの md をブロックに分解して編集する。
 *   - 編集後は markdownToPaperDraft で構造化フィールドを **派生** し直す
 *     (mechanics_json / 機密 synthetic 等の付随機能のため)。
 *
 * `paperDraftToMarkdown` の出力は従来 `discussion-paper.buildPaperSystem` が組み立てていた
 * 文面と **バイト等価** になるよう揃える (= 既存 Discord 経路の挙動を変えない)。
 *
 * 本モジュールは DB / LLM に依存しない純関数のみ (単体テスト可)。
 */

import type { FlowTag } from "./tags.js";
import type { MechanicSummary } from "./investigate.js";
import type { RoundSummary } from "./discussion-paper.js";

/** markdown 本文を持つペーパー (構造化フィールド + 本文 md)。 */
export interface PaperContent {
  theme: string;
  tags: FlowTag[];
  supplement: string;
  mechanics: MechanicSummary[];
  /** Web の固定フォーム用: ゲームタイトル、またはゲームでない場合の主目的。 */
  gameTitle?: string;
  /** Web の固定フォーム用: 議論したいテーマ。theme の表示値としても使う。 */
  discussionTheme?: string;
  /** Web の固定フォーム用: 何を議論してほしいかの本文。 */
  discussionContent?: string;
  /** Web の固定フォーム用: 仕様書/Anatomia/手入力由来のシステム・メカニクス説明。 */
  mechanicsContext?: string;
  /** Web の固定フォーム用: テーマについての補足情報。 */
  themeSupplement?: string;
}

const HEAD_THEME = "# 議題";
const HEAD_SUPPLEMENT = "# 観点補足";
const HEAD_MECHANICS = "# ゲームのメカニクス";
const HEAD_GAME_TITLE = "# ゲームタイトル（または主目的）";
const HEAD_DISCUSSION_THEME = "# 議論したいテーマ";
const HEAD_DISCUSSION_CONTENT = "# 議論内容";
const HEAD_MECHANICS_CONTEXT = "# システム/メカニクスの説明";
const HEAD_THEME_SUPPLEMENT = "# テーマについての補足情報";
const HEAD_EXTRACTED_MECHANICS = "## 抽出されたメカニクス";
const NO_MECHANICS = "(メカニクスデータなし)";

export interface PaperFixedFields {
  gameTitle: string;
  discussionTheme: string;
  discussionContent: string;
  mechanicsContext: string;
  themeSupplement: string;
}

/** 1 件のメカニクスを md 箇条書き 1 行にする (buildPaperSystem と同形)。 */
function mechanicLine(m: MechanicSummary): string {
  const affect = m.intended_affect ? ` → 期待感情: ${m.intended_affect}` : "";
  return `- **${m.name}**: ${m.description}${affect}`;
}

/** メカニクスリストを md 箇条書きにする (0 件はプレースホルダ)。 */
function mechanicsToMarkdown(mechanics: MechanicSummary[]): string {
  if (mechanics.length === 0) return NO_MECHANICS;
  return mechanics.map(mechanicLine).join("\n");
}

function hasFixedFields(content: PaperContent): boolean {
  return Boolean(
    content.gameTitle ||
      content.discussionTheme ||
      content.discussionContent ||
      content.mechanicsContext ||
      content.themeSupplement
  );
}

function fixedFieldsFromContent(content: PaperContent): PaperFixedFields {
  return {
    gameTitle: content.gameTitle?.trim() ?? "",
    discussionTheme: content.discussionTheme?.trim() || content.theme,
    discussionContent: content.discussionContent?.trim() ?? "",
    mechanicsContext: content.mechanicsContext?.trim() ?? "",
    themeSupplement: content.themeSupplement?.trim() ?? content.supplement,
  };
}

function stripGeneratedMechanics(text: string): string {
  const idx = text.indexOf(HEAD_EXTRACTED_MECHANICS);
  return (idx >= 0 ? text.slice(0, idx) : text).trim();
}

function fixedMechanicsSection(content: PaperContent): string {
  const chunks: string[] = [];
  const context = content.mechanicsContext?.trim();
  if (context) chunks.push(context);
  if (content.mechanics.length > 0) chunks.push(`${HEAD_EXTRACTED_MECHANICS}\n${mechanicsToMarkdown(content.mechanics)}`);
  if (chunks.length === 0) chunks.push(NO_MECHANICS);
  return chunks.join("\n\n");
}

function fixedPaperToMarkdown(content: PaperContent): string {
  const fields = fixedFieldsFromContent(content);
  return [
    `${HEAD_GAME_TITLE}\n${fields.gameTitle}`,
    `${HEAD_DISCUSSION_THEME}\n${fields.discussionTheme}`,
    `${HEAD_DISCUSSION_CONTENT}\n${fields.discussionContent}`,
    `${HEAD_MECHANICS_CONTEXT}\n${fixedMechanicsSection(content)}`,
    `${HEAD_THEME_SUPPLEMENT}\n${fields.themeSupplement}`,
  ].join("\n\n");
}

/**
 * 構造化ペーパー → 正本 markdown。
 * 出力形式は buildPaperSystem(議題 / 観点補足 / メカニクス) と揃える。
 */
export function paperDraftToMarkdown(content: PaperContent): string {
  if (hasFixedFields(content)) return fixedPaperToMarkdown(content);
  const sections: string[] = [`${HEAD_THEME}\n${content.theme}`];
  if (content.supplement) sections.push(`${HEAD_SUPPLEMENT}\n${content.supplement}`);
  sections.push(`${HEAD_MECHANICS}\n${mechanicsToMarkdown(content.mechanics)}`);
  return sections.join("\n\n");
}

/** md を `# 見出し` ごとのセクションに割る ({heading→本文行配列})。見出し前の本文は "" キーへ。 */
function splitSections(md: string): Map<string, string[]> {
  const sections = new Map<string, string[]>();
  let current = "";
  sections.set(current, []);
  for (const raw of md.split(/\r?\n/)) {
    const h1 = /^#\s+(.+?)\s*$/.exec(raw);
    if (h1) {
      current = `# ${h1[1]}`;
      if (!sections.has(current)) sections.set(current, []);
      continue;
    }
    const arr = sections.get(current);
    if (arr) arr.push(raw);
  }
  return sections;
}

/** セクション本文行配列をトリムして 1 文字列にする。 */
function sectionText(lines: string[] | undefined): string {
  if (!lines) return "";
  return lines.join("\n").trim();
}

/** 固定フォーム型の markdown から表示フィールドを取り出す。旧形式なら fallback で埋める。 */
export function paperFixedFieldsFromMarkdown(md: string, fallback: PaperContent): PaperFixedFields {
  const sections = splitSections(md);
  const mechanicsText = sectionText(sections.get(HEAD_MECHANICS_CONTEXT));
  return {
    gameTitle: sectionText(sections.get(HEAD_GAME_TITLE)) || fallback.gameTitle || "",
    discussionTheme:
      sectionText(sections.get(HEAD_DISCUSSION_THEME)) || fallback.discussionTheme || fallback.theme,
    discussionContent: sectionText(sections.get(HEAD_DISCUSSION_CONTENT)) || fallback.discussionContent || "",
    mechanicsContext: stripGeneratedMechanics(mechanicsText) || fallback.mechanicsContext || "",
    themeSupplement:
      sectionText(sections.get(HEAD_THEME_SUPPLEMENT)) || fallback.themeSupplement || fallback.supplement || "",
  };
}

/** メカニクス md 箇条書き 1 行を構造化する (`- **名前**: 説明 → 期待感情: x`)。 */
function parseMechanicLine(line: string): MechanicSummary | null {
  const m = /^-\s+\*\*(.+?)\*\*\s*:\s*(.*)$/.exec(line.trim());
  if (!m) return null;
  const name = m[1].trim();
  if (!name) return null;
  let rest = m[2].trim();
  let intended_affect: string | undefined;
  const affectIdx = rest.indexOf("→ 期待感情:");
  if (affectIdx >= 0) {
    intended_affect = rest.slice(affectIdx + "→ 期待感情:".length).trim() || undefined;
    rest = rest.slice(0, affectIdx).trim();
  }
  return { name, description: rest, ...(intended_affect ? { intended_affect } : {}) };
}

/** メカニクスセクション本文 → 構造化リスト (パース不能行は無視)。 */
function parseMechanics(text: string): MechanicSummary[] {
  if (!text || text === NO_MECHANICS) return [];
  const out: MechanicSummary[] = [];
  for (const line of text.split(/\r?\n/)) {
    const parsed = parseMechanicLine(line);
    if (parsed) out.push(parsed);
  }
  return out;
}

/**
 * 正本 markdown → 構造化ペーパー (派生)。
 *
 * 自由編集された md からも best-effort で議題 / 観点補足 / メカニクスを取り出す。
 * セクションが見つからない / パース不能なフィールドは fallback の値で補う
 * (= 編集で壊れても付随機能=mechanics_json/機密 synthetic を最大限保つ)。
 * tags は本文に含めない運用なので常に fallback.tags を保持する。
 */
export function markdownToPaperDraft(md: string, fallback: PaperContent): PaperContent {
  const sections = splitSections(md);
  const isFixed = sections.has(HEAD_GAME_TITLE) || sections.has(HEAD_DISCUSSION_THEME);
  if (isFixed) {
    const fields = paperFixedFieldsFromMarkdown(md, fallback);
    const mechanicsHas = sections.has(HEAD_MECHANICS_CONTEXT);
    const mechText = sectionText(sections.get(HEAD_MECHANICS_CONTEXT));
    const parsedMechanics = mechanicsHas ? parseMechanics(mechText) : fallback.mechanics;
    return {
      ...fallback,
      ...fields,
      theme: fields.discussionTheme || fallback.theme,
      tags: fallback.tags,
      supplement: sections.has(HEAD_THEME_SUPPLEMENT) ? fields.themeSupplement : fallback.supplement,
      mechanics: mechanicsHas ? parsedMechanics : fallback.mechanics,
    };
  }
  const themeText = sectionText(sections.get(HEAD_THEME));
  const supplementHas = sections.has(HEAD_SUPPLEMENT);
  const supplementText = sectionText(sections.get(HEAD_SUPPLEMENT));
  const mechHas = sections.has(HEAD_MECHANICS);
  const mechText = sectionText(sections.get(HEAD_MECHANICS));
  const parsedMechanics = mechHas ? parseMechanics(mechText) : fallback.mechanics;

  return {
    theme: themeText || fallback.theme,
    tags: fallback.tags,
    // 観点補足の見出しが在れば (空にされた場合も含め) その値、無ければ fallback。
    supplement: supplementHas ? supplementText : fallback.supplement,
    // メカニクス見出しが在ってパース 0 件なら「全削除」を尊重するが、見出し自体が
    // 消された自由編集では fallback を保つ (付随機能の取りこぼし防止)。
    mechanics: mechHas ? parsedMechanics : fallback.mechanics,
  };
}

const HEAD_PROGRESS = "# 議論の経過";
const HEAD_CONCLUSION = "# 結論";

/** base ブリーフから「議論の経過」以降 (進行ログ) を取り除く (再焼き直しの土台)。 */
export function stripProgress(bodyMd: string): string {
  const idx = bodyMd.indexOf(`\n${HEAD_PROGRESS}`);
  const base = idx >= 0 ? bodyMd.slice(0, idx) : bodyMd;
  return base.replace(/\s+$/g, "");
}

/**
 * base ブリーフ + 議論の経過 (ラウンドごとの まとめ / 止揚) + 結論 を 1 本の md に焼く。
 * 議論進行中に毎ラウンド呼び、`updatePaperBody` で discussion_paper.body_md を上書きする
 * (= ライブ UI で「ペーパーが更新されていく」)。base は既存の経過節を除いてから足し直す (冪等)。
 */
export function renderProgressMarkdown(
  baseBodyMd: string,
  rounds: readonly RoundSummary[],
  conclusion?: string
): string {
  const base = stripProgress(baseBodyMd);
  const parts: string[] = [base];

  if (rounds.length > 0) {
    const roundBlocks = rounds.map((r) => {
      const lines = [`## ラウンド ${r.round}`, r.summary.trim() || "(まとめなし)"];
      if (r.aufhebung.length > 0) {
        lines.push("", "**止揚 (アウフヘーベン)**:");
        for (const a of r.aufhebung) lines.push(`- ${a}`);
      }
      return lines.join("\n");
    });
    parts.push(`${HEAD_PROGRESS}\n\n${roundBlocks.join("\n\n")}`);
  }

  if (conclusion && conclusion.trim()) {
    parts.push(`${HEAD_CONCLUSION}\n${conclusion.trim()}`);
  }

  return parts.join("\n\n");
}
