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
import { asMechanicSource, type MechanicSummary } from "./investigate.js";
import type { RoundSummary } from "./discussion-paper.js";

/** markdown 本文を持つペーパー (構造化フィールド + 本文 md)。 */
export interface PaperContent {
  theme: string;
  tags: FlowTag[];
  supplement: string;
  mechanics: MechanicSummary[];
  /**
   * 論点 (賛否が本気で割れる争点、09-paper-gate)。md 正本の `# 論点` 節と round-trip する。
   * 人間がレビューゲートで編集でき、承認時 PaperOverride.issues として議論エンジンへ渡る。
   */
  issues?: string[];
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
/** 仮説メカニクス節 (source=llm を分離、08-paper-provenance)。 */
export const HEAD_HYPOTHESIS_MECHANICS = "# 仮説メカニクス (LLM抽出・未検証)";
/** 論点節 (争点分解の前倒し、09-paper-gate)。 */
export const HEAD_ISSUES = "# 論点";
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
function mechanicLine(m: MechanicSummary, withSource: boolean): string {
  const affect = m.intended_affect ? ` → 期待感情: ${m.intended_affect}` : "";
  // 出所併記 (08): curated/crawl 節では出所を明示する (undefined = 旧データ = curated 扱い)。
  const source = withSource ? ` （出所: ${m.source ?? "curated"}）` : "";
  return `- **${m.name}**: ${m.description}${affect}${source}`;
}

/** source=llm 以外 (= curated/crawl/旧データ) のメカニクス。「# ゲームのメカニクス」節に載る。 */
export function groundedMechanics(mechanics: readonly MechanicSummary[]): MechanicSummary[] {
  return mechanics.filter((m) => m.source !== "llm");
}

/** source=llm (LLM 増補 = 未検証の仮説) のメカニクス。「# 仮説メカニクス」節に分離する。 */
export function hypotheticalMechanics(mechanics: readonly MechanicSummary[]): MechanicSummary[] {
  return mechanics.filter((m) => m.source === "llm");
}

/** grounded メカニクスを md 箇条書き (出所併記) にする (0 件はプレースホルダ)。 */
export function groundedMechanicsMarkdown(mechanics: readonly MechanicSummary[]): string {
  const grounded = groundedMechanics(mechanics);
  if (grounded.length === 0) return NO_MECHANICS;
  return grounded.map((m) => mechanicLine(m, true)).join("\n");
}

/**
 * 仮説メカニクス節 (見出し込み) を返す。増補 0 件なら null (空節を作らない、08 受け入れ基準)。
 */
export function hypothesisSectionMarkdown(mechanics: readonly MechanicSummary[]): string | null {
  const hyp = hypotheticalMechanics(mechanics);
  if (hyp.length === 0) return null;
  return `${HEAD_HYPOTHESIS_MECHANICS}\n${hyp.map((m) => mechanicLine(m, false)).join("\n")}`;
}

/**
 * メカニクス本文 (grounded 箇条書き + 仮説節) を 1 文字列にする。
 * `# ゲームのメカニクス` 見出しの直下に置くと節構造がそのまま成立する
 * (buildPaperSystem の旧経路=formatMechanics と md 正本の両方がこれを使う)。
 */
export function mechanicsBodyMarkdown(mechanics: readonly MechanicSummary[]): string {
  const hypothesis = hypothesisSectionMarkdown(mechanics);
  const grounded = groundedMechanicsMarkdown(mechanics);
  return hypothesis ? `${grounded}\n\n${hypothesis}` : grounded;
}

/** 論点節 (見出し込み) を返す。0 件なら null (空節を作らない)。 */
export function issuesSectionMarkdown(issues: readonly string[] | undefined): string | null {
  const list = (issues ?? []).map((s) => s.trim()).filter(Boolean);
  if (list.length === 0) return null;
  return `${HEAD_ISSUES}\n${list.map((s, i) => `${i + 1}. ${s}`).join("\n")}`;
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
  const grounded = groundedMechanics(content.mechanics);
  if (grounded.length > 0) {
    chunks.push(`${HEAD_EXTRACTED_MECHANICS}\n${groundedMechanicsMarkdown(content.mechanics)}`);
  }
  if (chunks.length === 0) chunks.push(NO_MECHANICS);
  return chunks.join("\n\n");
}

function fixedPaperToMarkdown(content: PaperContent): string {
  const fields = fixedFieldsFromContent(content);
  const issues = issuesSectionMarkdown(content.issues);
  const hypothesis = hypothesisSectionMarkdown(content.mechanics);
  return [
    `${HEAD_GAME_TITLE}\n${fields.gameTitle}`,
    `${HEAD_DISCUSSION_THEME}\n${fields.discussionTheme}`,
    `${HEAD_DISCUSSION_CONTENT}\n${fields.discussionContent}`,
    ...(issues ? [issues] : []),
    `${HEAD_MECHANICS_CONTEXT}\n${fixedMechanicsSection(content)}`,
    ...(hypothesis ? [hypothesis] : []),
    `${HEAD_THEME_SUPPLEMENT}\n${fields.themeSupplement}`,
  ].join("\n\n");
}

/**
 * 構造化ペーパー → 正本 markdown。
 * 出力形式は buildPaperSystem(議題 / 観点補足 / 論点 / メカニクス / 仮説メカニクス) と揃える。
 */
export function paperDraftToMarkdown(content: PaperContent): string {
  if (hasFixedFields(content)) return fixedPaperToMarkdown(content);
  const sections: string[] = [`${HEAD_THEME}\n${content.theme}`];
  if (content.supplement) sections.push(`${HEAD_SUPPLEMENT}\n${content.supplement}`);
  const issues = issuesSectionMarkdown(content.issues);
  if (issues) sections.push(issues);
  // メカニクス本文 (grounded + 仮説節) — 仮説は source=llm のみ、0 件なら節を出さない。
  sections.push(`${HEAD_MECHANICS}\n${mechanicsBodyMarkdown(content.mechanics)}`);
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

/** 行末の出所併記 `（出所: curated）` (半角括弧も許容) を取り出して除去する。 */
function extractSourceSuffix(text: string): { rest: string; source?: MechanicSummary["source"] } {
  const m = /[（(]\s*出所\s*[:：]\s*([a-z]+)\s*[）)]\s*$/i.exec(text);
  if (!m) return { rest: text };
  const source = asMechanicSource(m[1].toLowerCase());
  if (!source) return { rest: text };
  return { rest: text.slice(0, m.index).trim(), source };
}

/** メカニクス md 箇条書き 1 行を構造化する (`- **名前**: 説明 → 期待感情: x （出所: y）`)。 */
function parseMechanicLine(
  line: string,
  forcedSource?: MechanicSummary["source"]
): MechanicSummary | null {
  const m = /^-\s+\*\*(.+?)\*\*\s*:\s*(.*)$/.exec(line.trim());
  if (!m) return null;
  const name = m[1].trim();
  if (!name) return null;
  // 出所併記 (08) を先に剥がす → 期待感情 → 説明 の順で分解する。
  const { rest: noSource, source: parsedSource } = extractSourceSuffix(m[2].trim());
  let rest = noSource;
  let intended_affect: string | undefined;
  const affectIdx = rest.indexOf("→ 期待感情:");
  if (affectIdx >= 0) {
    intended_affect = rest.slice(affectIdx + "→ 期待感情:".length).trim() || undefined;
    rest = rest.slice(0, affectIdx).trim();
  }
  const source = forcedSource ?? parsedSource;
  return {
    name,
    description: rest,
    ...(intended_affect ? { intended_affect } : {}),
    ...(source ? { source } : {}),
  };
}

/**
 * メカニクスセクション本文 → 構造化リスト (パース不能行は無視)。
 * @param forcedSource 節名から確定する出所 (仮説節 = "llm")。行内の出所併記より優先。
 */
function parseMechanics(text: string, forcedSource?: MechanicSummary["source"]): MechanicSummary[] {
  if (!text || text === NO_MECHANICS) return [];
  const out: MechanicSummary[] = [];
  for (const line of text.split(/\r?\n/)) {
    const parsed = parseMechanicLine(line, forcedSource);
    if (parsed) out.push(parsed);
  }
  return out;
}

/**
 * 論点セクション本文 → issues[]。`1. 論点` / `- 論点` の両形式を受ける。
 * 末尾の `(両論: ○)` 注記 (議論適性ゲートの表示) は剥がして素の論点文を返す。
 */
function parseIssues(text: string): string[] {
  if (!text) return [];
  const out: string[] = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    const m = /^(?:\d+[.)]\s+|[-*]\s+)(.+)$/.exec(line);
    if (!m) continue;
    const issue = m[1].replace(/\s*[（(]両論\s*[:：][^)）]*[)）]\s*$/, "").trim();
    if (issue) out.push(issue);
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

  // 仮説メカニクス節 (source=llm)。節が在ればその内容が正、無ければ「仮説なし」
  // (増補 0 件なら節を出さない仕様の round-trip)。ただし grounded 節も無い完全自由編集では
  // fallback を丸ごと保つ (下の mergeMechanics 参照)。
  const hypHas = sections.has(HEAD_HYPOTHESIS_MECHANICS);
  const hypMechanics = hypHas
    ? parseMechanics(sectionText(sections.get(HEAD_HYPOTHESIS_MECHANICS)), "llm")
    : [];
  // 論点節: 見出しが在れば (空にされた場合も含め) その値、無ければ fallback。
  const issuesHas = sections.has(HEAD_ISSUES);
  const parsedIssues = issuesHas ? parseIssues(sectionText(sections.get(HEAD_ISSUES))) : undefined;

  /** grounded 節の有無/内容と仮説節から mechanics を合成する共通則。 */
  const mergeMechanics = (groundedHas: boolean, groundedText: string): MechanicSummary[] => {
    // どちらの見出しも消された自由編集では fallback を保つ (付随機能の取りこぼし防止)。
    if (!groundedHas && !hypHas) return fallback.mechanics;
    // grounded 見出しが在ればパース結果 (0 件 = 全削除を尊重)、無ければ fallback の grounded 分。
    const grounded = groundedHas ? parseMechanics(groundedText) : groundedMechanics(fallback.mechanics);
    return [...grounded, ...hypMechanics];
  };

  if (isFixed) {
    const fields = paperFixedFieldsFromMarkdown(md, fallback);
    const mechanicsHas = sections.has(HEAD_MECHANICS_CONTEXT);
    const mechText = sectionText(sections.get(HEAD_MECHANICS_CONTEXT));
    return {
      ...fallback,
      ...fields,
      theme: fields.discussionTheme || fallback.theme,
      tags: fallback.tags,
      supplement: sections.has(HEAD_THEME_SUPPLEMENT) ? fields.themeSupplement : fallback.supplement,
      mechanics: mergeMechanics(mechanicsHas, mechText),
      issues: issuesHas ? parsedIssues : fallback.issues,
    };
  }
  const themeText = sectionText(sections.get(HEAD_THEME));
  const supplementHas = sections.has(HEAD_SUPPLEMENT);
  const supplementText = sectionText(sections.get(HEAD_SUPPLEMENT));
  const mechHas = sections.has(HEAD_MECHANICS);
  const mechText = sectionText(sections.get(HEAD_MECHANICS));

  return {
    theme: themeText || fallback.theme,
    tags: fallback.tags,
    // 観点補足の見出しが在れば (空にされた場合も含め) その値、無ければ fallback。
    supplement: supplementHas ? supplementText : fallback.supplement,
    // メカニクス見出しが在ってパース 0 件なら「全削除」を尊重するが、見出し自体が
    // 消された自由編集では fallback を保つ (付随機能の取りこぼし防止)。
    mechanics: mergeMechanics(mechHas, mechText),
    issues: issuesHas ? parsedIssues : fallback.issues,
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
