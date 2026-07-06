/**
 * ディスカッションペーパー レビューゲート (議論開始前の人手レビュー & 調整) の共有コア。
 *
 * 議論/改善フローは情報ゲート (クロール) のあと、すぐにペルソナへペーパーを配って
 * 議論を始める。ここはその「直前」に人間が **ペーパー (議題ブリーフ) と集めた情報を
 * 確認し、自然文で調整 → 承認してから議論を始める** ためのコア。
 *
 * トランスポート非依存 (Discord スレッド / Web UI の両方が使う):
 *   - buildPaperDraft  : investigate でメカニクス + 観点補足を組み、本文 md と情報サマリを付ける。
 *   - applyPaperEdit   : 自然文の調整指示を LLM でペーパーに反映する (構造化編集 → 本文 md 再生成)。
 *   - reviewBlock      : 本文 md の 1 ブロックを LLM で改稿提案する (Notion 風編集の補佐, 適用しない)。
 *   - gatherEvidence   : ブロックの根拠となる外部の声を集約し挿入用の段落を提案する (クロール補佐)。
 *   - coercePaperDraft : Web フォーム / 本文 md を PaperDraft に正規化する (本文 md 優先)。
 *   - renderPaperReview: ペーパー + 情報サマリを人間向け markdown にする。
 *   - isApprovalText   : 「開始」「承認」等の承認語かどうか。
 *
 * 本文は markdown を正本とする (ハイブリッド源泉, paper-markdown.ts)。構造化フィールド
 * (mechanics 等) は md から派生し直して付随機能 (mechanics_json / 機密 synthetic) に使う。
 * 確定したペーパーは director の runFlow(paperOverride) に bodyMd 込みで渡し、investigate を
 * 省いてその内容 (各 LLM が md を直接参照) で議論を回す。
 */

import type { LLMClient } from "../persona-engine/llm/client.js";
import type { FlowTag } from "./tags.js";
import { paperSupplement } from "./tags.js";
import {
  asMechanicSource,
  investigateTheme,
  type MechanicSummary,
  type YoutubeSearchFn,
} from "./investigate.js";
import { enrichMechanics } from "./mechanic-extract.js";
import type { ContextVoice } from "./discussion-paper.js";
import {
  paperDraftToMarkdown,
  markdownToPaperDraft,
  groundedMechanics,
  hypotheticalMechanics,
  type PaperContent,
  type PaperFixedFields,
} from "./paper-markdown.js";
import { getBlockText } from "./paper-blocks.js";
import { assessDebatability, annotatedIssues, type DebatabilityResult } from "./debatability.js";

/** 有効な観点タグ (編集で受理する集合)。 */
export const VALID_FLOW_TAGS: readonly FlowTag[] = ["機密", "内部", "運用", "開発"];

/** 情報サマリのサンプル件数 / 件数カウントの上限。 */
const SAMPLE_LIMIT = 9;
const COUNT_LIMIT = 50;

/**
 * 人間レビュー対象のペーパー草案 (= 議題ブリーフ)。
 * discussion_paper に永続する前の可変ドラフトで、承認時に runFlow(paperOverride) へ渡す。
 * 本文 (bodyMd) を正本とし、構造化フィールドはそこから派生する (ハイブリッド源泉)。
 */
export interface PaperDraft extends PaperContent {
  /** 議論ブリーフ本文の正本 markdown (Notion 風編集の編集対象)。 */
  bodyMd: string;
}

/** 構造化フィールドから本文 md を生成してドラフトにする。 */
export function withDerivedBody(content: PaperContent): PaperDraft {
  return { ...content, bodyMd: paperDraftToMarkdown(content) };
}

/** 本文 md から構造化フィールドを派生してドラフトにする (md を正本に保つ)。 */
export function withDerivedStructure(bodyMd: string, fallback: PaperContent): PaperDraft {
  return { ...markdownToPaperDraft(bodyMd, fallback), bodyMd };
}

/** ペーパーと併せて見せる「集めた情報」のサマリ。 */
export interface PaperReviewInfo {
  /** テーマに紐づく外部の声 (クロール材料) の件数。COUNT_LIMIT で頭打ち (= "N+")。 */
  voiceCount: number;
  /** 件数が上限に達して頭打ちか (表示で "+" を付ける)。 */
  countCapped: boolean;
  /** 数件のサンプル (出所付き・個人仮名)。 */
  samples: Array<{ content: string; source: string }>;
  /** AI が基本的なゲーム内容を把握できているかの事前確認。 */
  understanding?: PaperUnderstanding;
  /** 議論適性ゲート (09) の評価結果 (ゲート有効時のみ)。 */
  debatability?: DebatabilityResult;
  /** LLM チェック結果から生成した、編集者向けの修正提案。 */
  fixSuggestions?: PaperFixSuggestion[];
  /** LLM が把握しているゲームメカニクス知識の確認結果。 */
  mechanicsKnowledge?: PaperMechanicsKnowledge;
  /** ユーザの声を LLM で仮想生成して補助シミュレーションできるかの示唆。 */
  voiceSimulation?: PaperVoiceSimulation;
}

export interface PaperUnderstanding {
  ok: boolean;
  rationale: string;
  missingQuestions: string[];
}

export interface PaperFixSuggestion {
  title: string;
  reason: string;
  suggestedChange: string;
  /** Web UI でこの提案を本文へ反映した時刻。 */
  appliedAt?: number;
  /** プレースホルダ入力後、実際に本文へ追記した内容。 */
  appliedText?: string;
}

export interface PaperMechanicsKnowledge {
  ok: boolean;
  confidence: "low" | "medium" | "high";
  summary: string;
  knownMechanics: string[];
  missingQuestions: string[];
}

export interface PaperVoiceSimulation {
  possible: boolean;
  confidence: "low" | "medium" | "high";
  summary: string;
  caveat: string;
  nextAction: string;
}

export interface BuildPaperDraftDeps {
  youtubeSearch?: YoutubeSearchFn;
  youtubeMaxComments?: number;
  gamesDir?: string;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  /** メカニクス LLM 増補に使う LLM (省略時は増補しない)。 */
  llm?: LLMClient;
  /** 基本理解の確認に使う LLM。省略時は llm、どちらも無ければ決定論フォールバック。 */
  understandingLlm?: LLMClient;
  /** メカニクスの目標件数 (llm 指定時のみ有効。既定 30)。 */
  mechanicsTarget?: number;
  /** 増補に使うモデル ("" / 未指定なら LLM 既定)。 */
  enrichModel?: string;
  /**
   * 事前情報として先頭に差し込む追加メカニクス (Anatomia 由来など)。
   * investigate の結果より前に置き、名前で重複排除する (先勝ち = Anatomia を優先)。
   */
  extraMechanics?: MechanicSummary[];
  /** Web 固定フォーム由来の入力。指定時は本文 md を固定セクションで生成する。 */
  seed?: Partial<PaperFixedFields>;
  /**
   * 議論適性ゲート (09)。指定時は情報整備後・人間レビュー前に 3 検査を走らせ、
   * 論点 (`# 論点` 節) を草案に前倒しし、評価結果を info.debatability に載せる。
   * llm は呼び出し側で withCostLog 済みを渡す (resolveDebatabilityGate)。
   * 未指定 = ゲート無効 → 現行挙動と完全一致。
   */
  debatability?: {
    llm: LLMClient;
    minArmableIssues: number;
    /** 評価の差し替え (テスト用)。 */
    assess?: typeof assessDebatability;
  };
  warn?: (msg: string) => void;
}

/** メカニクスを名前で重複排除して連結する (先勝ち)。 */
export function mergeMechanics(
  primary: readonly MechanicSummary[],
  secondary: readonly MechanicSummary[]
): MechanicSummary[] {
  const seen = new Set<string>();
  const out: MechanicSummary[] = [];
  for (const m of [...primary, ...secondary]) {
    const key = m.name.trim().toLowerCase();
    if (!key || seen.has(key)) continue;
    seen.add(key);
    out.push(m);
  }
  return out;
}

/** 1 件の外部の声を表示用サンプルに丸める (長文は切り詰め)。 */
function toSample(v: ContextVoice): { content: string; source: string } {
  const body = v.content.length > 120 ? `${v.content.slice(0, 120)}…` : v.content;
  return { content: body, source: v.source };
}

function normalizeSeed(theme: string, seed?: Partial<PaperFixedFields>): PaperFixedFields | null {
  if (!seed) return null;
  const gameTitle = seed.gameTitle?.trim() ?? "";
  const discussionTheme = seed.discussionTheme?.trim() || theme;
  const discussionContent = seed.discussionContent?.trim() ?? "";
  const mechanicsContext = seed.mechanicsContext?.trim() ?? "";
  const themeSupplement = seed.themeSupplement?.trim() ?? "";
  return { gameTitle, discussionTheme, discussionContent, mechanicsContext, themeSupplement };
}

function deterministicUnderstanding(
  fields: PaperFixedFields,
  mechanics: readonly MechanicSummary[],
  voices: readonly ContextVoice[]
): PaperUnderstanding {
  const missingQuestions: string[] = [];
  if (!fields.gameTitle.trim()) {
    missingQuestions.push("ゲームタイトル、またはプロジェクトの主目的を補足してください。");
  }
  if (!fields.discussionTheme.trim()) {
    missingQuestions.push("何について議論したいか、テーマを1つに絞って補足してください。");
  }
  if (!fields.discussionContent.trim()) {
    missingQuestions.push("議論で判断したい論点、前提、迷っている案を補足してください。");
  }
  if (!fields.mechanicsContext.trim() && mechanics.length === 0) {
    missingQuestions.push("基本ループ、主要システム、操作、報酬、制約などゲーム内容が分かる説明を補足してください。");
  }
  const hasExternalContext = voices.length > 0 || mechanics.length > 0;
  const ok = missingQuestions.length === 0 || (missingQuestions.length <= 1 && hasExternalContext);
  return {
    ok,
    rationale: ok
      ? "入力内容と収集済み情報から、議論に必要な基本的なゲーム内容を把握できます。"
      : "議論の前提となるゲーム内容が不足しているため、補足があると論点が安定します。",
    missingQuestions: ok ? [] : missingQuestions,
  };
}

export async function assessPaperUnderstanding(
  fields: PaperFixedFields,
  mechanics: readonly MechanicSummary[],
  voices: readonly ContextVoice[],
  llm?: LLMClient,
  opts: { model?: string; warn?: (msg: string) => void } = {}
): Promise<PaperUnderstanding> {
  const fallback = deterministicUnderstanding(fields, mechanics, voices);
  if (!llm) return fallback;

  const system =
    "あなたはゲーム議論を始める前の確認担当です。入力されたディスカッションペーパーを読み、" +
    "AIが基本的なゲーム内容を理解して議論できるか判定します。情報が足りない場合は、ユーザに補足してほしい質問を返します。" +
    'JSON のみで {"ok":boolean,"rationale":string,"missingQuestions":string[]} を返してください。';
  const prompt = JSON.stringify(
    {
      fields,
      extractedMechanics: mechanics.slice(0, 20),
      externalVoiceCount: voices.length,
      externalVoiceSamples: voices.slice(0, 5).map((v) => v.content.slice(0, 160)),
    },
    null,
    2
  );

  try {
    const r = await llm.invoke({ system, prompt, model: opts.model });
    if (!r.ok) {
      opts.warn?.(`paper-understanding LLM エラー: ${r.error}`);
      return fallback;
    }
    const parsed = extractJsonObject(r.text);
    if (!parsed || typeof parsed !== "object") return fallback;
    const o = parsed as Record<string, unknown>;
    const missing = Array.isArray(o.missingQuestions)
      ? o.missingQuestions.filter((q): q is string => typeof q === "string" && q.trim() !== "").map((q) => q.trim())
      : [];
    return {
      ok: typeof o.ok === "boolean" ? o.ok : missing.length === 0,
      rationale:
        typeof o.rationale === "string" && o.rationale.trim()
          ? o.rationale.trim()
          : fallback.rationale,
      missingQuestions: missing,
    };
  } catch (e) {
    opts.warn?.(`paper-understanding LLM 例外: ${(e as Error).message}`);
    return fallback;
  }
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
  const seed = normalizeSeed(theme, deps.seed);
  const investigation = await investigateTheme({
    theme,
    tags,
    gamesDir: deps.gamesDir,
    youtubeSearch: deps.youtubeSearch,
    youtubeMaxComments: deps.youtubeMaxComments,
    warn: deps.warn,
  });

  const voices = deps.listExternalVoices ? deps.listExternalVoices([theme], COUNT_LIMIT) : [];

  // 事前情報 (Anatomia 由来など) を先頭に置き、investigate 結果と名前で重複排除する。
  const base = mergeMechanics(deps.extraMechanics ?? [], investigation.mechanics);

  // メカニクスを LLM で目標件数まで増補 (感想を根拠に。llm 未指定なら base の件数のまま)。
  let mechanics = base;
  if (deps.llm) {
    mechanics = await enrichMechanics({
      theme,
      existing: base,
      voices,
      llm: deps.llm,
      target: deps.mechanicsTarget ?? 30,
      model: deps.enrichModel || undefined,
      warn: deps.warn,
    });
  }

  const supplement = seed
    ? [paperSupplement(tags), seed.themeSupplement].filter((s) => s.trim()).join("\n")
    : paperSupplement(tags);
  const baseContent: PaperContent = {
    theme: seed?.discussionTheme || theme,
    tags: [...tags],
    supplement,
    mechanics,
    ...(seed ?? {}),
  };

  // 議論適性ゲート (09): 情報ゲート (量) の後段・人間レビューの前に「質」を評価する。
  // 論点分解を前倒しして `# 論点` 節を草案に載せる (人間がブロック編集で調整できる)。
  // LLM 失敗は degraded=true で議論を止めない (assessDebatability 側で明示)。
  let debatability: DebatabilityResult | undefined;
  if (deps.debatability) {
    const assess = deps.debatability.assess ?? assessDebatability;
    debatability = await assess({
      theme,
      paperMd: paperDraftToMarkdown(baseContent),
      voices,
      llm: deps.debatability.llm,
      minArmableIssues: deps.debatability.minArmableIssues,
      warn: deps.warn,
    });
    const issues = annotatedIssues(debatability);
    if (issues.length > 0) baseContent.issues = issues;
  }

  const draft: PaperDraft = withDerivedBody(baseContent);
  const understanding = seed
    ? await assessPaperUnderstanding(seed, mechanics, voices, deps.understandingLlm ?? deps.llm, {
        model: deps.enrichModel || undefined,
        warn: deps.warn,
      })
    : undefined;
  const info: PaperReviewInfo = {
    voiceCount: voices.length,
    countCapped: voices.length >= COUNT_LIMIT,
    samples: voices.slice(0, SAMPLE_LIMIT).map(toSample),
    understanding,
    debatability,
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

/** 任意の JSON 値を MechanicSummary に正規化する (name 必須)。source は有効値のみ保持。 */
function normalizeMechanic(raw: unknown): MechanicSummary | null {
  if (!raw || typeof raw !== "object") return null;
  const m = raw as Record<string, unknown>;
  const name = typeof m.name === "string" ? m.name.trim() : "";
  if (!name) return null;
  const affect = typeof m.intended_affect === "string" ? m.intended_affect.trim() : "";
  const source = asMechanicSource(m.source);
  return {
    name,
    description: typeof m.description === "string" ? m.description.trim() : "",
    ...(affect ? { intended_affect: affect } : {}),
    ...(source ? { source } : {}),
  };
}

/** 任意の JSON 値を issues[] に正規化する (文字列のみ・trim・空除外)。 */
function sanitizeIssues(raw: unknown): string[] | undefined {
  if (!Array.isArray(raw)) return undefined;
  const issues = raw
    .filter((v): v is string => typeof v === "string")
    .map((s) => s.trim())
    .filter(Boolean);
  return issues;
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
    gameTitle: draft.gameTitle ?? "",
    discussionTheme: draft.discussionTheme ?? draft.theme,
    discussionContent: draft.discussionContent ?? "",
    mechanicsContext: draft.mechanicsContext ?? "",
    themeSupplement: draft.themeSupplement ?? draft.supplement,
    theme: draft.theme,
    tags: draft.tags,
    supplement: draft.supplement,
    issues: draft.issues ?? [],
    mechanics: draft.mechanics.map((m) => ({
      name: m.name,
      description: m.description,
      intended_affect: m.intended_affect ?? "",
      source: m.source ?? "curated",
    })),
  };

  const system =
    "あなたは議論ブリーフ (ディスカッションペーパー) の編集者です。" +
    "ユーザの調整指示に従い、ペーパーを編集して **JSON のみ** で返してください。" +
    "指示に関係ないフィールドは現行値のまま保持します。" +
    `tags に使えるのは ${VALID_FLOW_TAGS.join("/")} のみ。` +
    "mechanics の source (curated/llm/crawl = 出所ラベル) は指示がない限り現行値を保持します。" +
    "issues は議論の論点 (賛否が割れる争点) の配列です。" +
    "出力スキーマ: " +
    '{"gameTitle":string,"discussionTheme":string,"discussionContent":string,"mechanicsContext":string,"themeSupplement":string,' +
    '"theme":string,"tags":string[],"supplement":string,"issues":string[],' +
    '"mechanics":[{"name":string,"description":string,"intended_affect":string,"source":string}],' +
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

  const gameTitle = typeof obj.gameTitle === "string" ? obj.gameTitle.trim() : draft.gameTitle;
  const discussionTheme =
    typeof obj.discussionTheme === "string" && obj.discussionTheme.trim()
      ? obj.discussionTheme.trim()
      : draft.discussionTheme;
  const discussionContent =
    typeof obj.discussionContent === "string" ? obj.discussionContent.trim() : draft.discussionContent;
  const mechanicsContext =
    typeof obj.mechanicsContext === "string" ? obj.mechanicsContext.trim() : draft.mechanicsContext;
  const themeSupplement =
    typeof obj.themeSupplement === "string" ? obj.themeSupplement.trim() : draft.themeSupplement;
  const theme =
    discussionTheme ||
    (typeof obj.theme === "string" && obj.theme.trim() ? obj.theme.trim() : draft.theme);
  const supplement =
    themeSupplement ??
    (typeof obj.supplement === "string" ? obj.supplement.trim() : draft.supplement);
  const tags = "tags" in obj ? sanitizeTags(obj.tags) : draft.tags;
  const mechanics = Array.isArray(obj.mechanics)
    ? obj.mechanics.map(normalizeMechanic).filter((m): m is MechanicSummary => m !== null)
    : draft.mechanics;
  const issues = "issues" in obj ? sanitizeIssues(obj.issues) : draft.issues;
  const changeSummary =
    typeof obj.changeSummary === "string" && obj.changeSummary.trim()
      ? obj.changeSummary.trim()
      : "ペーパーを更新しました。";

  return {
    // 構造化編集後に本文 md を再生成する (md を正本に保つ)。
    draft: withDerivedBody({
      theme,
      tags,
      supplement,
      mechanics,
      issues,
      gameTitle,
      discussionTheme,
      discussionContent,
      mechanicsContext,
      themeSupplement,
    }),
    changeSummary,
    applied: true,
  };
}

/**
 * 外部入力 (Web フォームの直接編集 JSON / 本文 md) を PaperDraft に正規化する。
 * 本文 md が来ていればそれを正本とし構造化フィールドを派生する (Notion 風編集の確定経路)。
 * 欠落/不正フィールドは fallback (現行ドラフト) で補う。tags は有効タグのみ。
 */
export function coercePaperDraft(raw: unknown, fallback: PaperDraft): PaperDraft {
  if (!raw || typeof raw !== "object") return fallback;
  const o = raw as Record<string, unknown>;
  // tags は本文 md に含めないので、本文 md 経路でも別途上書きを受ける。
  const tags = "tags" in o ? sanitizeTags(o.tags) : fallback.tags;
  // 本文 md があれば最優先 (md を正本に保つ)。tags は別管理なので上書き反映する。
  if (typeof o.bodyMd === "string" && o.bodyMd.trim()) {
    return withDerivedStructure(o.bodyMd, { ...fallback, tags });
  }
  const theme = typeof o.theme === "string" && o.theme.trim() ? o.theme.trim() : fallback.theme;
  const supplement = typeof o.supplement === "string" ? o.supplement : fallback.supplement;
  const mechanics = Array.isArray(o.mechanics)
    ? o.mechanics.map(normalizeMechanic).filter((m): m is MechanicSummary => m !== null)
    : fallback.mechanics;
  const issues = "issues" in o ? sanitizeIssues(o.issues) : fallback.issues;
  return withDerivedBody({ theme, tags, supplement, mechanics, issues });
}

/** ペーパー + 情報サマリを人間向け markdown にする (Discord 投稿 / Web 表示で共有)。 */
export function renderPaperReview(draft: PaperDraft, info: PaperReviewInfo): string {
  const lines: string[] = [];
  lines.push("📝 **ディスカッションペーパー (議論ブリーフ) — 確認してください**");
  lines.push("");
  lines.push(`**議題**: ${draft.theme}`);
  if (draft.tags.length) lines.push(`**観点タグ**: ${draft.tags.join(" / ")}`);
  if (draft.supplement) lines.push(`**観点補足**: ${draft.supplement}`);

  // 論点 (09): 争点分解の前倒し。人間が返信/編集で調整できる。
  if (draft.issues && draft.issues.length > 0) {
    lines.push("");
    lines.push("**論点**:");
    draft.issues.forEach((issue, i) => lines.push(`${i + 1}. ${issue}`));
  }

  lines.push("");
  const grounded = groundedMechanics(draft.mechanics);
  const hypothetical = hypotheticalMechanics(draft.mechanics);
  if (grounded.length) {
    lines.push("**ゲームのメカニクス**:");
    for (const m of grounded) {
      const affect = m.intended_affect ? ` → 期待感情: ${m.intended_affect}` : "";
      lines.push(`- ${m.name}: ${m.description}${affect} （出所: ${m.source ?? "curated"}）`);
    }
  } else {
    lines.push("**ゲームのメカニクス**: (なし)");
  }
  // 仮説メカニクス (08): LLM 増補分はバッジ付きで分離表示 (議論開始前に削れる)。
  if (hypothetical.length) {
    lines.push("");
    lines.push("🧪 **仮説メカニクス (LLM抽出・未検証)**:");
    for (const m of hypothetical) {
      const affect = m.intended_affect ? ` → 期待感情: ${m.intended_affect}` : "";
      lines.push(`- ${m.name}: ${m.description}${affect}`);
    }
  }

  lines.push("");
  const countLabel = `${info.voiceCount}${info.countCapped ? "+" : ""}`;
  lines.push(`**集めた情報**: 外部の声 ${countLabel} 件`);
  for (const s of info.samples) {
    lines.push(`- 「${s.content}」(出所: ${s.source})`);
  }

  // 議論適性ゲート (09): 議論不適なら再提案を併記する (人間が最終決定・強行可)。
  const d = info.debatability;
  if (d && !d.degraded) {
    lines.push("");
    if (d.debatable) {
      lines.push(`✅ ${d.message}`);
    } else {
      lines.push(`⚠️ ${d.message}`);
      if (d.recommendation) {
        const label = d.recommendation.flow === "sparring" ? "壁打ち" : "学習";
        lines.push(`💡 **提案**: このテーマは「${label}」が向いています。${d.recommendation.reason}`);
        lines.push("(このまま議論を開始することもできます — 判断はお任せします)");
      }
    }
  } else if (d?.degraded) {
    lines.push("");
    lines.push(`⚠️ ${d.message}`);
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

/** 「戻す」語かどうか (Discord ペーパーレビューで 1 手前に戻す。Web の ↶戻すと対応)。 */
export function isRevertText(text: string): boolean {
  const t = text.trim().toLowerCase().replace(/[。.!！\s]+$/, "");
  if (!t) return false;
  const exact = new Set(["戻す", "戻して", "もどす", "取り消し", "取消", "undo", "revert", "↶", "↩"]);
  return exact.has(t);
}

// ── Notion 風ブロック編集の補佐 (Web) ────────────────────────────────────────

export interface BlockReviewResult {
  blockId: string;
  /** 改稿前の本文 (見つからなければ "")。 */
  original: string;
  /** LLM の改稿案 (失敗時は original のまま)。 */
  proposed: string;
  /** 何をどう直したかの一言 (日本語)。 */
  rationale: string;
  /** 改稿案を得られたか (false = ブロック不在 / LLM 失敗で original のまま)。 */
  ok: boolean;
}

export interface ReviewBlockArgs {
  bodyMd: string;
  blockId: string;
  /** 任意の調整指示 (例「もっと具体的に」)。空なら「議論しやすく明確に」既定。 */
  instruction?: string;
  llm: LLMClient;
  model?: string;
  warn?: (msg: string) => void;
}

/**
 * 本文 md の 1 ブロックを LLM で改稿提案する (適用はしない=UI が diff 提示して採否)。
 * ブロックが見つからない / LLM 失敗時は ok=false で original を返す (本文を壊さない)。
 */
export async function reviewBlock(args: ReviewBlockArgs): Promise<BlockReviewResult> {
  const { bodyMd, blockId, llm } = args;
  const original = getBlockText(bodyMd, blockId);
  if (original == null) {
    return { blockId, original: "", proposed: "", rationale: "対象ブロックが見つかりません。", ok: false };
  }
  const instruction = (args.instruction ?? "").trim() || "議論しやすく明確に整える";
  const system =
    "あなたは議論ブリーフ (ディスカッションペーパー) の編集補佐です。" +
    "渡された markdown ブロック 1 つを、議題ブリーフとして読みやすく改稿します。" +
    "見出し記号 (#) や箇条書き記号 (-) のブロック種別は保ち、意味を勝手に増やさない。" +
    "**JSON のみ** で返す。スキーマ: " +
    '{"proposed":string,"rationale":string}。' +
    "proposed は改稿後のブロック本文 (markdown)、rationale は変更点を 1 文 (日本語)。" +
    "前置きやコードフェンスは付けない。";
  const prompt =
    `# 調整方針\n${instruction}\n\n# 対象ブロック (現行)\n${original}\n\n` +
    `# 参考: ブリーフ全文\n${bodyMd}\n\n上記ブロックの改稿案を JSON で返してください。`;

  let result;
  try {
    result = await llm.invoke({ system, prompt, model: args.model });
  } catch (e) {
    args.warn?.(`reviewBlock LLM 例外: ${(e as Error).message}`);
    return { blockId, original, proposed: original, rationale: "改稿に失敗しました (LLM 例外)。", ok: false };
  }
  if (!result.ok) {
    args.warn?.(`reviewBlock LLM エラー: ${result.error}`);
    return { blockId, original, proposed: original, rationale: "改稿に失敗しました。", ok: false };
  }
  const parsed = extractJsonObject(result.text) as Record<string, unknown> | null;
  const proposed =
    parsed && typeof parsed.proposed === "string" && parsed.proposed.trim()
      ? parsed.proposed.trim()
      : "";
  if (!proposed) {
    return { blockId, original, proposed: original, rationale: "改稿案を取得できませんでした。", ok: false };
  }
  const rationale =
    parsed && typeof parsed.rationale === "string" && parsed.rationale.trim()
      ? parsed.rationale.trim()
      : "ブロックを改稿しました。";
  return { blockId, original, proposed, rationale, ok: true };
}

export interface EvidenceResult {
  /** 集めた外部の声 (出所付き・個人仮名)。 */
  voices: Array<{ content: string; source: string }>;
  /** ブロックに挿入できる根拠段落の提案 (LLM 要約 or 箇条書き)。 */
  suggestion: string;
}

export interface GatherEvidenceArgs {
  /** 根拠を集める対象 (ブロック本文 or テーマ語)。検索語に使う。 */
  topic: string;
  /** RAG: 既に集めた KG から外部の声を引く (read-only, web 経路の listExternalVoices)。 */
  listExternalVoices: (terms: string[], limit: number) => ContextVoice[];
  /** 提案段落を要約生成する LLM (省略時は箇条書きをそのまま提案)。 */
  llm?: LLMClient;
  model?: string;
  limit?: number;
  warn?: (msg: string) => void;
}

/**
 * ブロックの根拠となる外部の声を集約し、挿入用の段落を提案する (クロール補佐)。
 * KG への書込みはせず、収集済みの声を RAG で引くだけ (編集中の単一writer衝突を避ける)。
 * 声が無ければ空の suggestion を返す。
 */
export async function gatherEvidence(args: GatherEvidenceArgs): Promise<EvidenceResult> {
  const limit = args.limit ?? 12;
  const raw = args.listExternalVoices([args.topic], limit);
  const voices = raw.slice(0, limit).map((v) => ({
    content: v.content.length > 200 ? `${v.content.slice(0, 200)}…` : v.content,
    source: v.source,
  }));
  if (voices.length === 0) return { voices, suggestion: "" };

  // 箇条書きの素の根拠 (LLM 無し時はこれを提案)。
  const bullets = voices.map((v) => `- 「${v.content}」（出所: ${v.source}）`).join("\n");
  if (!args.llm) return { voices, suggestion: `## 集めた根拠 (外部の声)\n${bullets}` };

  const system =
    "あなたは議論ブリーフの編集補佐です。集めた外部の声を踏まえ、議題に関する観点の" +
    "根拠段落を中立的にまとめます。markdown の段落 (箇条書き可) のみを返す (前置き不要)。";
  const prompt = `# 観点\n${args.topic}\n\n# 集めた外部の声\n${bullets}\n\n根拠段落をまとめてください。`;
  try {
    const r = await args.llm.invoke({ system, prompt, model: args.model });
    if (r.ok && r.text.trim()) return { voices, suggestion: r.text.trim() };
  } catch (e) {
    args.warn?.(`gatherEvidence LLM 例外: ${(e as Error).message}`);
  }
  return { voices, suggestion: `## 集めた根拠 (外部の声)\n${bullets}` };
}
