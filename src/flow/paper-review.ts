/**
 * ディスカッションペーパー レビューゲート (議論開始前の人手レビュー & 調整) の共有コア。
 *
 * 議論/改善フローは情報ゲート (クロール) のあと、すぐにペルソナへペーパーを配って
 * 議論を始める。ここはその「直前」に人間が **ペーパー (議題ブリーフ) と集めた情報を
 * 確認し、自然文で調整 → 承認してから議論を始める** ためのコア。
 *
 * トランスポート非依存 (Discord スレッド / Web UI の両方が使う):
 *   - buildPaperDraft  : investigate でメカニクス + 観点補足を組み、集めた情報サマリを付ける。
 *   - applyPaperEdit   : 自然文の調整指示を LLM でペーパーに反映する (構造化編集)。
 *   - renderPaperReview: ペーパー + 情報サマリを人間向け markdown にする。
 *   - isApprovalText   : 「開始」「承認」等の承認語かどうか。
 *
 * 確定したペーパーは director の runFlow(paperOverride) に渡し、investigate を省いて
 * その内容で議論を回す (= 人間が直したメカニクス/観点で議論する)。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowTag } from "./tags.js";
import { paperSupplement } from "./tags.js";
import { investigateTheme, type MechanicSummary, type YoutubeSearchFn } from "./investigate.js";
import { enrichMechanics } from "./mechanic-extract.js";
import type { ContextVoice } from "./discussion-paper.js";

/** 有効な観点タグ (編集で受理する集合)。 */
export const VALID_FLOW_TAGS: readonly FlowTag[] = ["機密", "内部", "運用", "開発"];

/** 情報サマリのサンプル件数 / 件数カウントの上限。 */
const SAMPLE_LIMIT = 9;
const COUNT_LIMIT = 50;

/**
 * 人間レビュー対象のペーパー草案 (= 議題ブリーフ)。
 * discussion_paper に永続する前の可変ドラフトで、承認時に runFlow(paperOverride) へ渡す。
 */
export interface PaperDraft {
  theme: string;
  tags: FlowTag[];
  supplement: string;
  mechanics: MechanicSummary[];
}

/** ペーパーと併せて見せる「集めた情報」のサマリ。 */
export interface PaperReviewInfo {
  /** テーマに紐づく外部の声 (クロール材料) の件数。COUNT_LIMIT で頭打ち (= "N+")。 */
  voiceCount: number;
  /** 件数が上限に達して頭打ちか (表示で "+" を付ける)。 */
  countCapped: boolean;
  /** 数件のサンプル (出所付き・個人仮名)。 */
  samples: Array<{ content: string; source: string }>;
}

export interface BuildPaperDraftDeps {
  youtubeSearch?: YoutubeSearchFn;
  youtubeMaxComments?: number;
  gamesDir?: string;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  /** メカニクス LLM 増補に使う LLM (省略時は増補しない)。 */
  llm?: LLMClient;
  /** メカニクスの目標件数 (llm 指定時のみ有効。既定 30)。 */
  mechanicsTarget?: number;
  /** 増補に使うモデル ("" / 未指定なら LLM 既定)。 */
  enrichModel?: string;
  warn?: (msg: string) => void;
}

/** 1 件の外部の声を表示用サンプルに丸める (長文は切り詰め)。 */
function toSample(v: ContextVoice): { content: string; source: string } {
  const body = v.content.length > 120 ? `${v.content.slice(0, 120)}…` : v.content;
  return { content: body, source: v.source };
}

/**
 * テーマを investigate してペーパー草案 + 集めた情報サマリを組み立てる (永続化しない)。
 * メカニクスは investigateTheme から、観点補足はタグから、情報サマリは外部の声から。
 */
export async function buildPaperDraft(
  theme: string,
  tags: readonly FlowTag[],
  deps: BuildPaperDraftDeps
): Promise<{ draft: PaperDraft; info: PaperReviewInfo }> {
  const investigation = await investigateTheme({
    theme,
    tags,
    gamesDir: deps.gamesDir,
    youtubeSearch: deps.youtubeSearch,
    youtubeMaxComments: deps.youtubeMaxComments,
    warn: deps.warn,
  });

  const voices = deps.listExternalVoices ? deps.listExternalVoices([theme], COUNT_LIMIT) : [];

  // メカニクスを LLM で目標件数まで増補 (感想を根拠に。llm 未指定なら investigate の件数のまま)。
  let mechanics = investigation.mechanics;
  if (deps.llm) {
    mechanics = await enrichMechanics({
      theme,
      existing: investigation.mechanics,
      voices,
      llm: deps.llm,
      target: deps.mechanicsTarget ?? 30,
      model: deps.enrichModel || undefined,
      warn: deps.warn,
    });
  }

  const draft: PaperDraft = {
    theme,
    tags: [...tags],
    supplement: paperSupplement(tags),
    mechanics,
  };
  const info: PaperReviewInfo = {
    voiceCount: voices.length,
    countCapped: voices.length >= COUNT_LIMIT,
    samples: voices.slice(0, SAMPLE_LIMIT).map(toSample),
  };
  return { draft, info };
}

/** 文字列から最初の JSON オブジェクトを取り出す (コードフェンス/前置き混入に耐える)。 */
function extractJsonObject(raw: string): unknown {
  const start = raw.indexOf("{");
  const end = raw.lastIndexOf("}");
  if (start < 0 || end <= start) return null;
  try {
    return JSON.parse(raw.slice(start, end + 1));
  } catch {
    return null;
  }
}

/** 任意の JSON 値を MechanicSummary に正規化する (name 必須)。 */
function normalizeMechanic(raw: unknown): MechanicSummary | null {
  if (!raw || typeof raw !== "object") return null;
  const m = raw as Record<string, unknown>;
  const name = typeof m.name === "string" ? m.name.trim() : "";
  if (!name) return null;
  const affect = typeof m.intended_affect === "string" ? m.intended_affect.trim() : "";
  return {
    name,
    description: typeof m.description === "string" ? m.description.trim() : "",
    ...(affect ? { intended_affect: affect } : {}),
  };
}

/** 編集 LLM が返したタグ配列を有効タグだけに絞る。 */
function sanitizeTags(raw: unknown): FlowTag[] {
  if (!Array.isArray(raw)) return [];
  const set = new Set<FlowTag>();
  for (const t of raw) {
    if (typeof t === "string" && (VALID_FLOW_TAGS as readonly string[]).includes(t)) set.add(t as FlowTag);
  }
  return [...set];
}

export interface PaperEditResult {
  /** 反映後 (失敗時は元のまま) のドラフト。 */
  draft: PaperDraft;
  /** 何をどう変えたか (人間向け一言)。 */
  changeSummary: string;
  /** 指示を反映できたか。false = パース失敗等で変更なし。 */
  applied: boolean;
}

export interface ApplyPaperEditOpts {
  model?: string;
  warn?: (msg: string) => void;
}

/**
 * 自然文の調整指示を LLM でペーパーに反映する (構造化編集)。
 *
 * 指示例:「観点補足を初心者向けに」「メカニクスにガチャを追加」「議題をもっと具体的に」。
 * LLM には現行ペーパー + 指示を渡し、編集後の全文を JSON で返させる (各フィールドは短文)。
 * パース/検証に失敗したら **元のドラフトを保ち** applied=false を返す (議論ブリーフを壊さない)。
 */
export async function applyPaperEdit(
  draft: PaperDraft,
  instruction: string,
  llm: LLMClient,
  opts: ApplyPaperEditOpts = {}
): Promise<PaperEditResult> {
  const trimmed = instruction.trim();
  if (!trimmed) return { draft, changeSummary: "指示が空です。", applied: false };

  const current = {
    theme: draft.theme,
    tags: draft.tags,
    supplement: draft.supplement,
    mechanics: draft.mechanics.map((m) => ({
      name: m.name,
      description: m.description,
      intended_affect: m.intended_affect ?? "",
    })),
  };

  const system =
    "あなたは議論ブリーフ (ディスカッションペーパー) の編集者です。" +
    "ユーザの調整指示に従い、ペーパーを編集して **JSON のみ** で返してください。" +
    "指示に関係ないフィールドは現行値のまま保持します。" +
    `tags に使えるのは ${VALID_FLOW_TAGS.join("/")} のみ。` +
    "出力スキーマ: " +
    '{"theme":string,"tags":string[],"supplement":string,' +
    '"mechanics":[{"name":string,"description":string,"intended_affect":string}],' +
    '"changeSummary":string}。' +
    "changeSummary は変更点を 1 文 (日本語) で。前置きやコードフェンスは付けない。";

  const prompt =
    `# 現行ペーパー\n${JSON.stringify(current, null, 2)}\n\n` +
    `# 調整指示\n${trimmed}\n\n` +
    "上記指示を反映した編集後ペーパーを JSON で返してください。";

  let result;
  try {
    result = await llm.invoke({ system, prompt, model: opts.model });
  } catch (e) {
    opts.warn?.(`paper-edit LLM 例外: ${(e as Error).message}`);
    return { draft, changeSummary: "編集に失敗しました (LLM 例外)。言い換えて再度お試しください。", applied: false };
  }
  if (!result.ok) {
    opts.warn?.(`paper-edit LLM エラー: ${result.error}`);
    return { draft, changeSummary: "編集に失敗しました。言い換えて再度お試しください。", applied: false };
  }

  const parsed = extractJsonObject(result.text);
  if (!parsed || typeof parsed !== "object") {
    return { draft, changeSummary: "指示を反映できませんでした。言い換えてください。", applied: false };
  }
  const obj = parsed as Record<string, unknown>;

  const theme = typeof obj.theme === "string" && obj.theme.trim() ? obj.theme.trim() : draft.theme;
  const supplement = typeof obj.supplement === "string" ? obj.supplement.trim() : draft.supplement;
  const tags = "tags" in obj ? sanitizeTags(obj.tags) : draft.tags;
  const mechanics = Array.isArray(obj.mechanics)
    ? obj.mechanics.map(normalizeMechanic).filter((m): m is MechanicSummary => m !== null)
    : draft.mechanics;
  const changeSummary =
    typeof obj.changeSummary === "string" && obj.changeSummary.trim()
      ? obj.changeSummary.trim()
      : "ペーパーを更新しました。";

  return {
    draft: { theme, tags, supplement, mechanics },
    changeSummary,
    applied: true,
  };
}

/**
 * 外部入力 (Web フォームの直接編集 JSON 等) を PaperDraft に正規化する。
 * 欠落/不正フィールドは fallback (現行ドラフト) で補う。tags は有効タグのみ。
 */
export function coercePaperDraft(raw: unknown, fallback: PaperDraft): PaperDraft {
  if (!raw || typeof raw !== "object") return fallback;
  const o = raw as Record<string, unknown>;
  const theme = typeof o.theme === "string" && o.theme.trim() ? o.theme.trim() : fallback.theme;
  const supplement = typeof o.supplement === "string" ? o.supplement : fallback.supplement;
  const tags = "tags" in o ? sanitizeTags(o.tags) : fallback.tags;
  const mechanics = Array.isArray(o.mechanics)
    ? o.mechanics.map(normalizeMechanic).filter((m): m is MechanicSummary => m !== null)
    : fallback.mechanics;
  return { theme, tags, supplement, mechanics };
}

/** ペーパー + 情報サマリを人間向け markdown にする (Discord 投稿 / Web 表示で共有)。 */
export function renderPaperReview(draft: PaperDraft, info: PaperReviewInfo): string {
  const lines: string[] = [];
  lines.push("📝 **ディスカッションペーパー (議論ブリーフ) — 確認してください**");
  lines.push("");
  lines.push(`**議題**: ${draft.theme}`);
  if (draft.tags.length) lines.push(`**観点タグ**: ${draft.tags.join(" / ")}`);
  if (draft.supplement) lines.push(`**観点補足**: ${draft.supplement}`);

  lines.push("");
  if (draft.mechanics.length) {
    lines.push("**ゲームのメカニクス**:");
    for (const m of draft.mechanics) {
      const affect = m.intended_affect ? ` → 期待感情: ${m.intended_affect}` : "";
      lines.push(`- ${m.name}: ${m.description}${affect}`);
    }
  } else {
    lines.push("**ゲームのメカニクス**: (なし)");
  }

  lines.push("");
  const countLabel = `${info.voiceCount}${info.countCapped ? "+" : ""}`;
  lines.push(`**集めた情報**: 外部の声 ${countLabel} 件`);
  for (const s of info.samples) {
    lines.push(`- 「${s.content}」(出所: ${s.source})`);
  }

  return lines.join("\n");
}

/** 承認語かどうか (「開始」「承認」「approve」等)。調整指示と区別する。 */
export function isApprovalText(text: string): boolean {
  const t = text.trim().toLowerCase().replace(/[。.!！\s]+$/, "");
  if (!t) return false;
  const exact = new Set([
    "開始",
    "承認",
    "ok",
    "okay",
    "go",
    "approve",
    "approved",
    "start",
    "これで開始",
    "これでいい",
    "これでok",
    "それで開始",
    "オッケー",
    "おっけー",
    "👍",
    "✅",
  ]);
  return exact.has(t);
}
