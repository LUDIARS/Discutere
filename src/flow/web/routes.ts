/**
 * 議論フロー WebUI ルート (t7-entrypoints.md 経路 A)。
 *
 *   GET  /flow                       簡素な 1 ページ UI (テーマ + 議論タイプ + タグ)
 *   POST /api/flow/start             フロー起動 (theme, flow, tags) → dispatch
 *   POST /api/flow/:session/say      壁打ちのユーザ発話投入
 *   GET  /api/flow/:session/status   発話 + 結論をポーリング取得 (?since=<ms>)
 *
 * 認証は他の HTTP UI と同じく loopback 信頼。進行・発話・結論は flow_* テーブルに
 * 永続され、ブラウザがポーリングで取得する (web-chat と同じ思想)。
 */

import { Hono } from "hono";
import { randomUUID } from "node:crypto";

import type { LLMClient } from "../../persona-engine/llm/client.js";
import type { CascadeClients } from "../../crawler/sentiment/cascade.js";
import type { createCore } from "../../core/index.js";
import type { ContextVoice, FlowSessionScope, FlowSessionState } from "../discussion-paper.js";
import {
  getPaperBodyBySession,
  persistDraftPaper,
  getDraftPaper,
  getPaperSnapshot,
  getPaperReviewInfo,
  deleteFlowSession,
  listFlowSessions,
  setPaperDebatability,
  setPaperReviewInfo,
} from "../discussion-paper.js";
import { resolveDebatabilityGate } from "../information-gate-runner.js";

type Core = ReturnType<typeof createCore>;
import { getFlowDb } from "../db/connection.js";
import { dispatchFlow, parseFlowKind, type DispatchDeps, type FlowKind } from "../dispatch.js";
import type { SparringSession } from "../sparring.js";
import type { FlowTag } from "../tags.js";
import { composeDisplayName } from "../persona-display.js";
import type { FlowRole, FlowStance } from "../personas.js";
import type { AutoCrawlSpec } from "../learning-autocrawl.js";
import { analyzeSpecMechanics } from "../spec-analyze.js";
import { buildGithubSpecSource, fetchGithubSpecText, resolveSpecText } from "../spec-source.js";
import { getGitHubCliToken } from "../github-cli.js";
import { mechanicSummaryToEntry, type GameMechanicEntry } from "../games-md.js";
import type { LearningOpinion } from "../learning.js";
import { extractJsonArray } from "../mechanic-extract.js";
import {
  webPaperGateEnabled,
  buildAutoCrawlSpec,
  buildLearningCrawl,
  prepareInformationBeforeFlow,
  resolveYoutubeApiKey,
  type FlowWebDeps,
} from "./flow-prep.js";
import {
  buildPaperDraft,
  coercePaperDraft,
  reviewBlock,
  gatherEvidence,
  withDerivedStructure,
  type PaperFixSuggestion,
  type PaperMechanicsKnowledge,
  type PaperDraft,
  type PaperReviewInfo,
} from "../paper-review.js";
import { assessDebatability, annotatedIssues } from "../debatability.js";
import { splitBlocks, replaceBlock, insertBlockAfter } from "../paper-blocks.js";
import { paperDraftToMarkdown, paperFixedFieldsFromMarkdown, type PaperFixedFields } from "../paper-markdown.js";
import { appendRevision, canRevert, revertLast, listRevisions } from "../paper-revisions.js";
import { getConfig } from "../../config.js";
import { getAnatomiaMechanics, resolveAnatomiaSource } from "../anatomia/index.js";
import type { MechanicSummary } from "../investigate.js";
import { FLOW_HTML } from "./page.js";

let deps: FlowWebDeps | null = null;

/**
 * Anatomia 事前情報メカニクスを解決する (config 有効 + 対象指定時のみ)。
 * - config.flow.anatomia.enabled=false: 指定があっても無視 (warn)。
 * - 対象未指定: undefined (従来どおり investigate のみ)。
 * - 取得失敗 (bin 不在 / CLI 非 0 / JSON 不正) は throw を伝播させる (fail-fast・UI にエラー表示)。
 */
async function resolveAnatomiaExtraMechanics(
  theme: string,
  anatomiaProject: string,
  anatomiaRepo: string,
  llm: LLMClient,
  warn: (msg: string) => void
): Promise<MechanicSummary[] | undefined> {
  const source = resolveAnatomiaSource(anatomiaProject, anatomiaRepo);
  if (!source) return undefined;
  const cfg = getConfig().flow.anatomia;
  if (!cfg.enabled) {
    warn("Anatomia 連携は無効 (flow.anatomia.enabled=false) のため指定を無視します");
    return undefined;
  }
  const mechanics = await getAnatomiaMechanics(source, {
    binPath: cfg.binPath,
    autoDraft: cfg.autoDraft,
    timeoutMs: cfg.timeoutMs,
    refineModel: cfg.refineModel,
    llm,
    theme,
    warn,
  });
  warn(`Anatomia 由来メカニクス ${mechanics.length} 件を事前情報に追加`);
  return mechanics;
}

export function setFlowWebDeps(d: FlowWebDeps): void {
  deps = d;
}

/** flow=壁打ち の起動済みセッションを保持する (HTTP の後続発話を受けるため)。 */
const sparringSessions = new Map<string, SparringSession>();
/** discussion/improvement の完了状態 (status の done 判定用)。 */
const finished = new Set<string>();

/**
 * ペーパーレビュー待ち (sessionId → 草案 + 起動入力)。
 * /start で草案構築をバックグラウンド起動し、ready になったら ブラウザが /paper で取得して
 * 直接編集 → /paper/approve で議論を開始する。
 */
interface WebPaperReview {
  flow: FlowKind;
  rounds?: number;
  turnsPerRound?: number;
  /** 草案構築が完了したか (false の間はブラウザは待つ)。 */
  ready: boolean;
  /** 構築失敗時のメッセージ (あれば)。 */
  error?: string;
  draft?: PaperDraft;
  info?: PaperReviewInfo;
  /** 無操作の自動開始タイマー (timeoutMs>0 時のみ。調整があるたび張り直す)。 */
  timer?: ReturnType<typeof setTimeout>;
}
const paperReviews = new Map<string, WebPaperReview>();

/** 確定ペーパーで議論を起動する (承認エンドポイント / 自動開始タイマー 共通)。 */
function startApprovedFlow(
  sessionId: string,
  entry: WebPaperReview,
  finalPaper: PaperDraft,
  webDeps: FlowWebDeps
): void {
  if (entry.timer) clearTimeout(entry.timer);
  paperReviews.delete(sessionId);
  const dispatchDeps: DispatchDeps = {
    llm: webDeps.llm,
    listExternalVoices: webDeps.listExternalVoices,
    sentimentClients: webDeps.sentimentClients,
    gamesDir: webDeps.gamesDir,
    workspaceId: webDeps.workspaceId,
  };
  // 議論適性: 低のまま強行した場合は評価結果を discussion_paper に記録する (09 監査ログ)。
  const debatability = entry.info?.debatability;
  if (debatability && !debatability.degraded && !debatability.debatable) {
    try {
      setPaperDebatability(sessionId, debatability);
    } catch (e) {
      console.warn(`[flow-web] debatability 記録失敗 (議論は続行): ${(e as Error).message}`);
    }
  }
  void dispatchFlow(
    { theme: finalPaper.theme, tags: finalPaper.tags, flow: entry.flow, rounds: entry.rounds, turnsPerRound: entry.turnsPerRound },
    {
      ...dispatchDeps,
      sessionId,
      paperOverride: {
        mechanics: finalPaper.mechanics,
        supplement: finalPaper.supplement,
        bodyMd: finalPaper.bodyMd,
        // 承認済み論点 (09): ファシリテーター開幕プロンプトの参考として運ぶ。
        issues: finalPaper.issues,
        ...(debatability && !debatability.degraded
          ? {
              debatability: {
                debatable: debatability.debatable,
                armableBothCount: debatability.armableBothCount,
              },
            }
          : {}),
      },
    }
  )
    .catch((e) => console.warn(`[flow-web] ${entry.flow} 実行エラー: ${(e as Error).message}`))
    .finally(() => finished.add(sessionId));
}

/** 無操作の自動開始タイマーを (張り直して) 仕掛ける。timeoutMs<=0 なら何もしない。 */
function scheduleWebAutoApprove(sessionId: string, webDeps: FlowWebDeps): void {
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return;
  const timeoutMs = getConfig().flow.paperReview.timeoutMs;
  if (timeoutMs <= 0) return;
  if (entry.timer) clearTimeout(entry.timer);
  entry.timer = setTimeout(() => {
    const e = paperReviews.get(sessionId);
    if (!e || !e.draft) return;
    console.log(`[flow-web/paper ${sessionId}] 無操作のため草案のまま自動開始`);
    startApprovedFlow(sessionId, e, e.draft, webDeps);
  }, timeoutMs);
  entry.timer.unref?.();
}

const VALID_REVIEW_TAGS = new Set<FlowTag>(["機密", "内部", "運用", "開発"]);

function stringField(body: Record<string, unknown>, key: string): string {
  const v = body[key];
  return typeof v === "string" ? v.trim() : "";
}

function sanitizeReviewTags(raw: unknown, fallback: readonly FlowTag[] = []): FlowTag[] {
  const values = Array.isArray(raw) ? raw : fallback;
  const tags = new Set<FlowTag>();
  for (const t of values) {
    if (typeof t === "string" && VALID_REVIEW_TAGS.has(t as FlowTag)) tags.add(t as FlowTag);
  }
  return [...tags];
}

/** 論点フィールド (1 行 1 論点のテキスト or 配列) を issues[] に正規化する。 */
function parseIssuesField(raw: unknown): string[] | undefined {
  const values = Array.isArray(raw)
    ? raw.filter((v): v is string => typeof v === "string")
    : typeof raw === "string"
      ? raw.split(/\r?\n/)
      : undefined;
  if (!values) return undefined;
  return values
    .map((s) => s.replace(/^(?:\d+[.)]\s+|[-*]\s+)/, "").trim())
    .filter(Boolean);
}

function fixedSeedFromBody(body: Record<string, unknown>): Partial<PaperFixedFields> | undefined {
  const seed = {
    gameTitle: stringField(body, "gameTitle"),
    discussionTheme: stringField(body, "discussionTheme"),
    discussionContent: stringField(body, "discussionContent"),
    mechanicsContext: stringField(body, "mechanicsContext"),
    themeSupplement: stringField(body, "themeSupplement"),
  };
  return Object.values(seed).some(Boolean) ? seed : undefined;
}

async function resolveAdditionalSpecText(
  body: {
    specUrl?: unknown;
    githubRepoUrl?: unknown;
    githubPath?: unknown;
    githubRef?: unknown;
  },
  warn: (msg: string) => void
): Promise<string> {
  const parts: string[] = [];
  const specUrl = typeof body.specUrl === "string" ? body.specUrl.trim() : "";
  const githubRepoUrl = typeof body.githubRepoUrl === "string" ? body.githubRepoUrl.trim() : "";
  const githubPath = typeof body.githubPath === "string" ? body.githubPath.trim() : "";
  const githubRef = typeof body.githubRef === "string" ? body.githubRef.trim() : "";
  let githubToken: string | null | undefined;
  const token = async () => {
    if (githubToken !== undefined) return githubToken;
    githubToken = await getGitHubCliToken();
    return githubToken;
  };

  if (specUrl) {
    try {
      const needsGithubToken = /(?:github\.com|raw\.githubusercontent\.com|git@github\.com)/i.test(specUrl);
      parts.push(
        await resolveSpecText(specUrl, {
          allowLocalPath: true,
          githubToken: needsGithubToken ? await token() : undefined,
        })
      );
    } catch (e) {
      warn(`[flow-web/spec] spec source read failed (${specUrl}): ${(e as Error).message}`);
    }
  }

  if (githubRepoUrl) {
    try {
      const source = buildGithubSpecSource(githubRepoUrl, githubPath, githubRef);
      if (!source) throw new Error("GitHub repository URL could not be parsed");
      if (!source.path) throw new Error("GitHub path is required");
      parts.push((await fetchGithubSpecText(source, { githubToken: await token() })).text);
    } catch (e) {
      warn(`[flow-web/github] GitHub file read failed (${githubRepoUrl} ${githubPath}): ${(e as Error).message}`);
    }
  }

  return parts.join("\n\n");
}

function mergeMechanicsContext(
  seed: Partial<PaperFixedFields> | undefined,
  ...texts: Array<string | undefined>
): Partial<PaperFixedFields> | undefined {
  const values: string[] = [];
  for (const text of [seed?.mechanicsContext, ...texts]) {
    const trimmed = text?.trim();
    if (trimmed && !values.includes(trimmed)) values.push(trimmed);
  }
  if (values.length === 0) return seed;
  return { ...(seed ?? {}), mechanicsContext: values.join("\n\n") };
}

async function synthesizeLearningOpinions(args: {
  theme: string;
  mechanics: GameMechanicEntry[];
  mechanicsContext?: string;
  llm: LLMClient;
  warn: (msg: string) => void;
}): Promise<LearningOpinion[]> {
  const mechanicsText =
    args.mechanics.length > 0
      ? args.mechanics
          .slice(0, 20)
          .map((m, i) => `${i + 1}. ${m.name}: ${m.description ?? ""}`)
          .join("\n")
      : args.mechanicsContext?.slice(0, 6000) || args.theme;
  const system =
    "You create plausible pre-release player feedback for game design learning. Return JSON only. Do not include real personal data.";
  const prompt =
    `Game/theme:\n${args.theme}\n\nMechanics/design notes:\n${mechanicsText}\n\n` +
    "Create 8 anonymized user voice snippets as a JSON array. Balance positive, negative, and mixed reactions. " +
    'Each item must be {"content":"..."} and the content should start with "[synthetic] ".';
  try {
    const res = await args.llm.invoke({ system, prompt, maxTokens: 2500 });
    if (!res.ok) {
      args.warn(`synthetic user voice generation failed: ${res.error}`);
      return [];
    }
    const arr = extractJsonArray(res.text);
    if (!arr) return [];
    return arr
      .map((raw): LearningOpinion | null => {
        if (!raw || typeof raw !== "object") return null;
        const content = typeof (raw as Record<string, unknown>).content === "string"
          ? ((raw as Record<string, unknown>).content as string).trim()
          : "";
        return content ? { content, postedAt: Date.now() } : null;
      })
      .filter((item): item is LearningOpinion => item !== null)
      .slice(0, 12);
  } catch (e) {
    args.warn(`synthetic user voice generation threw: ${(e as Error).message}`);
    return [];
  }
}

function flowThemeFromSeed(seed: Partial<PaperFixedFields> | undefined, legacyTheme: string): string {
  if (!seed) return legacyTheme;
  return [seed.gameTitle, seed.discussionTheme || legacyTheme].filter((v) => v && v.trim()).join(" / ");
}

function readOptionalInt(raw: unknown, min: number, max: number): number | undefined {
  const n = typeof raw === "number" ? raw : typeof raw === "string" && raw.trim() !== "" ? Number(raw) : undefined;
  if (!Number.isFinite(n)) return undefined;
  return Math.min(max, Math.max(min, Math.trunc(n as number)));
}

function defaultPaperInfo(existing?: PaperReviewInfo): PaperReviewInfo {
  return existing ?? { voiceCount: 0, countCapped: false, samples: [] };
}

function storedPaperReviewInfo(sessionId: string): PaperReviewInfo | undefined {
  const info = getPaperReviewInfo(sessionId);
  return info && typeof info === "object" ? (info as PaperReviewInfo) : undefined;
}

function savePaperReviewInfo(sessionId: string, info: PaperReviewInfo): void {
  try {
    setPaperReviewInfo(sessionId, info);
  } catch (e) {
    console.warn(`[flow-web/paper ${sessionId}] review info 保存失敗: ${(e as Error).message}`);
  }
}

const REVIEW_INFO_SAMPLE_LIMIT = 9;
const DEBATABILITY_VOICE_LIMIT = 50;

function reviewInfoSamples(voices: readonly ContextVoice[]): PaperReviewInfo["samples"] {
  return voices.slice(0, REVIEW_INFO_SAMPLE_LIMIT).map((v) => ({
    content: v.content.length > 120 ? `${v.content.slice(0, 120)}...` : v.content,
    source: v.source,
  }));
}

function manualDebatabilityGate(
  kind: FlowKind,
  sessionId: string,
  llm: LLMClient
): { llm: LLMClient; minArmableIssues: number } | undefined {
  if (kind !== "discussion" && kind !== "improvement") return undefined;
  return (
    resolveDebatabilityGate({ kind, sessionId, llm }) ?? {
      llm,
      minArmableIssues: getConfig().flow.debatability.minArmableIssues,
    }
  );
}

function extractJsonObject(raw: string): Record<string, unknown> | null {
  const start = raw.indexOf("{");
  const end = raw.lastIndexOf("}");
  if (start < 0 || end <= start) return null;
  try {
    const parsed = JSON.parse(raw.slice(start, end + 1));
    return parsed && typeof parsed === "object" && !Array.isArray(parsed)
      ? (parsed as Record<string, unknown>)
      : null;
  } catch {
    return null;
  }
}

function shortString(raw: unknown, fallback = ""): string {
  return typeof raw === "string" ? raw.trim().slice(0, 2000) : fallback;
}

function stringArray(raw: unknown, limit: number): string[] {
  return Array.isArray(raw)
    ? raw
        .filter((v): v is string => typeof v === "string" && v.trim() !== "")
        .map((v) => v.trim().slice(0, 1000))
        .slice(0, limit)
    : [];
}

async function suggestFixesForPaper(args: {
  draft: PaperDraft;
  info: PaperReviewInfo;
  llm: LLMClient;
  model?: string;
  warn: (msg: string) => void;
}): Promise<PaperFixSuggestion[]> {
  const hasFindings =
    args.info.understanding?.ok === false ||
    (args.info.debatability && !args.info.debatability.degraded && !args.info.debatability.debatable);
  if (!hasFindings) return [];
  const system =
    "You review a Japanese discussion paper before debate. Return JSON only. " +
    "Create concrete fix suggestions from the provided check findings. Do not rewrite the whole paper.";
  const prompt = JSON.stringify(
    {
      paperMd: args.draft.bodyMd,
      understanding: args.info.understanding ?? null,
      debatability: args.info.debatability ?? null,
      expectedShape: [
        { title: "短い見出し", reason: "指摘理由", suggestedChange: "編集者が追記・修正すべき具体内容" },
      ],
    },
    null,
    2
  );
  try {
    const res = await args.llm.invoke({ system, prompt, model: args.model, maxTokens: 1800 });
    if (!res.ok) {
      args.warn(`paper-fix-suggestions LLM error: ${res.error}`);
      return [];
    }
    const arr = extractJsonArray(res.text);
    if (!arr) return [];
    return arr
      .map((raw): PaperFixSuggestion | null => {
        if (!raw || typeof raw !== "object") return null;
        const o = raw as Record<string, unknown>;
        const title = shortString(o.title);
        const reason = shortString(o.reason);
        const suggestedChange = shortString(o.suggestedChange);
        return title && suggestedChange ? { title, reason, suggestedChange } : null;
      })
      .filter((item): item is PaperFixSuggestion => item !== null)
      .slice(0, 5);
  } catch (e) {
    args.warn(`paper-fix-suggestions threw: ${(e as Error).message}`);
    return [];
  }
}

async function checkMechanicsKnowledge(args: {
  draft: PaperDraft;
  fields: PaperFixedFields;
  llm: LLMClient;
  model?: string;
  warn: (msg: string) => void;
}): Promise<PaperMechanicsKnowledge> {
  const system =
    "You check whether you have enough game mechanics knowledge to support a debate. " +
    "Return JSON only. Do not invent certainty; mark low confidence when details are missing.";
  const prompt = JSON.stringify(
    {
      gameTitle: args.fields.gameTitle,
      discussionTheme: args.fields.discussionTheme || args.draft.theme,
      providedMechanicsContext: args.fields.mechanicsContext,
      extractedMechanics: args.draft.mechanics.slice(0, 30),
      expectedShape: {
        ok: true,
        confidence: "low|medium|high",
        summary: "理解状況の短い説明",
        knownMechanics: ["把握しているメカニクス"],
        missingQuestions: ["不足している確認事項"],
      },
    },
    null,
    2
  );
  try {
    const res = await args.llm.invoke({ system, prompt, model: args.model, maxTokens: 1800 });
    if (!res.ok) throw new Error(res.error);
    const obj = extractJsonObject(res.text);
    if (!obj) throw new Error("no JSON object");
    const confidence = obj.confidence === "high" || obj.confidence === "medium" ? obj.confidence : "low";
    const knownMechanics = stringArray(obj.knownMechanics, 12);
    const missingQuestions = stringArray(obj.missingQuestions, 8);
    return {
      ok: typeof obj.ok === "boolean" ? obj.ok : confidence !== "low" && missingQuestions.length === 0,
      confidence,
      summary: shortString(obj.summary, "メカニクス理解度を確認しました。"),
      knownMechanics,
      missingQuestions,
    };
  } catch (e) {
    args.warn(`mechanics-knowledge check failed: ${(e as Error).message}`);
    return {
      ok: false,
      confidence: "low",
      summary: "メカニクス知識確認に失敗しました。仕様書・ゲーム内容・主要ループを補足してください。",
      knownMechanics: [],
      missingQuestions: ["ゲームの基本ループ、主要システム、報酬、制約を補足してください。"],
    };
  }
}

export const flowRoutes = new Hono();

flowRoutes.get("/flow", (c) => c.html(FLOW_HTML));

flowRoutes.post("/api/flow/start", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps; // module-level let を closure で使うため const に束ねる
  const body = (await c.req.json().catch(() => ({}))) as {
    theme?: unknown;
    gameTitle?: unknown;
    discussionTheme?: unknown;
    discussionContent?: unknown;
    mechanicsContext?: unknown;
    themeSupplement?: unknown;
    flow?: unknown;
    tags?: unknown;
    rounds?: unknown;
    turnsPerRound?: unknown;
    opponent?: unknown;
    learningSource?: unknown;
    learningQuery?: unknown;
    learningAppId?: unknown;
    learningUrls?: unknown;
    learningSubreddit?: unknown;
    learningBalanced?: unknown;
    specText?: unknown;
    specUrl?: unknown;
    githubRepoUrl?: unknown;
    githubPath?: unknown;
    githubRef?: unknown;
    anatomiaProject?: unknown;
    anatomiaRepo?: unknown;
  };
  const legacyTheme = typeof body.theme === "string" ? body.theme.trim() : "";
  const additionalSpecText = await resolveAdditionalSpecText(body, (m) => console.warn(m));
  const baseSeed = fixedSeedFromBody(body as Record<string, unknown>);
  const seed = mergeMechanicsContext(
    baseSeed,
    typeof body.specText === "string" ? body.specText : undefined,
    additionalSpecText
  );
  const theme = flowThemeFromSeed(seed, legacyTheme);
  const fixedMode = !!baseSeed;
  // Anatomia 事前情報: 登録済みプロジェクト名 or リポ絶対パス (どちらか)。config で有効時のみ使う。
  const anatomiaProject = typeof body.anatomiaProject === "string" ? body.anatomiaProject.trim() : "";
  const anatomiaRepo = typeof body.anatomiaRepo === "string" ? body.anatomiaRepo.trim() : "";
  const flowLabel = typeof body.flow === "string" ? body.flow : "";
  const tags = Array.isArray(body.tags)
    ? (body.tags.filter((t) => typeof t === "string") as FlowTag[])
    : [];
  // 議論ごとのラウンド/ターン数 (任意)。runFlow が config 既定にフォールバック + 上限クランプする。
  const rounds = readOptionalInt(body.rounds, 1, 10);
  const turnsPerRound = readOptionalInt(body.turnsPerRound, 1, 20);
  // G: 壁打ち相手 (カンマ区切りの名前/ID)。sparring のみ反映。
  const opponentPersonaIds =
    typeof body.opponent === "string" && body.opponent.trim() !== ""
      ? body.opponent.split(",").map((s) => s.trim()).filter(Boolean)
      : undefined;

  if (!theme) return c.json({ ok: false, error: "テーマは必須です" }, 400);
  if (fixedMode && (!seed?.gameTitle || !seed?.discussionTheme)) {
    return c.json({ ok: false, error: "ゲームタイトル(または主目的)と議論したいテーマは必須です" }, 400);
  }
  const kind = parseFlowKind(flowLabel);
  if (!kind) return c.json({ ok: false, error: "議論タイプ (必須) を選択してください" }, 400);

  const dispatchDeps: DispatchDeps = {
    llm: deps.llm,
    listExternalVoices: deps.listExternalVoices,
    sentimentClients: deps.sentimentClients,
    gamesDir: deps.gamesDir,
    workspaceId: deps.workspaceId,
  };

  // 壁打ち: セッションを起動して登録 (対話継続)
  if (kind === "sparring") {
    const result = await dispatchFlow(
      { theme, tags, flow: kind, scene: `web:flow-${randomUUID().slice(0, 8)}`, opponentPersonaIds },
      dispatchDeps
    );
    if (result.kind !== "sparring") return c.json({ ok: false, error: "internal" }, 500);
    sparringSessions.set(result.session.sessionId, result.session);
    return c.json({ ok: true, kind, sessionId: result.session.sessionId });
  }

  // 学習: 短時間で完走するため await して結果を返す。
  //   - 自動収集モード (① 類似ゲーム): opinions 未供給でも横断クロール。
  //   - 仕様書解析 (②/③ spec): 貼付/アップロード本文 (specText) + URL/パス取得 (specUrl) を
  //     まとめて LLM 解析 → mechanics として記録。Web は loopback 信頼でローカルパス読みを許可。
  if (kind === "learning") {
    if (!deps.openCore) return c.json({ ok: false, error: "learning は Core 未設定のため不可" }, 400);
    // youtube API キーは runtime 解決 (tuning UI / gcloud secret 経由)。
    const youtubeApiKey = await resolveYoutubeApiKey(webDeps);
    const learningCrawl = buildLearningCrawl({ ...body, youtubeApiKey });
    const inlineSpec =
      seed?.mechanicsContext?.trim() ||
      (typeof body.specText === "string" && body.specText.trim() ? body.specText.trim() : "");
    const specText = inlineSpec;
    const core = deps.openCore();
    try {
      let mechanics: GameMechanicEntry[] = [];
      if (specText) {
        mechanics = await analyzeSpecMechanics({
          theme,
          specText,
          llm: webDeps.llm,
          warn: (m) => console.warn(`[flow-web/spec] ${m}`),
        });
      }
      const extraMechanics = await resolveAnatomiaExtraMechanics(
        theme,
        anatomiaProject,
        anatomiaRepo,
        webDeps.llm,
        (m) => console.warn(`[flow-web/anatomia learning] ${m}`)
      );
      if (extraMechanics?.length) {
        mechanics = [...mechanics, ...extraMechanics.map(mechanicSummaryToEntry)];
      }
      const result = await dispatchFlow(
        { theme, tags, flow: kind },
        { ...dispatchDeps, core, learningCrawl, mechanics: mechanics.length ? mechanics : undefined }
      );
      if (result.kind !== "learning") return c.json({ ok: false, error: "internal" }, 500);
      let finalResult = result.result;
      let syntheticOpinions = 0;
      if (finalResult.opinionsRecorded === 0 && finalResult.crawledImported === 0) {
        const opinions = await synthesizeLearningOpinions({
          theme,
          mechanics,
          mechanicsContext: specText,
          llm: webDeps.llm,
          warn: (m) => console.warn(`[flow-web/learning] ${m}`),
        });
        syntheticOpinions = opinions.length;
        if (opinions.length > 0) {
          const synthetic = await dispatchFlow(
            { theme, tags, flow: kind },
            { ...dispatchDeps, core, opinions, mechanics: undefined, learningCrawl: undefined }
          );
          if (synthetic.kind === "learning") {
            finalResult = {
              ...finalResult,
              opinionsRecorded: finalResult.opinionsRecorded + synthetic.result.opinionsRecorded,
              polarityBreakdown: {
                positive:
                  (finalResult.polarityBreakdown.positive ?? 0) +
                  (synthetic.result.polarityBreakdown.positive ?? 0),
                negative:
                  (finalResult.polarityBreakdown.negative ?? 0) +
                  (synthetic.result.polarityBreakdown.negative ?? 0),
                neutral:
                  (finalResult.polarityBreakdown.neutral ?? 0) +
                  (synthetic.result.polarityBreakdown.neutral ?? 0),
              },
            };
          }
        }
      }
      return c.json({ ok: true, kind, sessionId: finalResult.gameSlug, result: finalResult, syntheticOpinions });
    } finally {
      core.close?.();
    }
  }

  // 議論 / 改善: sessionId を先に発番し、バックグラウンドで完走させてポーリングで追う。
  // 学習データが不足していれば、議論を始める前に指定ソースでクロール → 取込する (事前学習の UI 化)。
  const sessionId = randomUUID();
  const autoCrawlSpec = buildAutoCrawlSpec(body);

  // ペーパー編集ゲート (Web 正規フロー): 情報整備後に草案を作り、ブラウザの編集/承認を待つ。
  if (webPaperGateEnabled()) {
    paperReviews.set(sessionId, { flow: kind, rounds, turnsPerRound, ready: false });
    void (async () => {
      await prepareInformationBeforeFlow(kind, theme, tags, sessionId, autoCrawlSpec, webDeps);
      const richness = getConfig().flow.paperRichness;
      // Anatomia 事前情報: 対象が指定され config 有効なら、ソース解析由来のメカニクスを先頭に差す。
      const extraMechanics = await resolveAnatomiaExtraMechanics(
        theme,
        anatomiaProject,
        anatomiaRepo,
        webDeps.llm,
        (m) => console.warn(`[flow-web/anatomia ${sessionId}] ${m}`)
      );
      const { draft, info } = await buildPaperDraft(theme, tags, {
        gamesDir: webDeps.gamesDir,
        listExternalVoices: webDeps.listExternalVoices,
        llm: richness.enrichMechanics ? webDeps.llm : undefined,
        understandingLlm: webDeps.llm,
        mechanicsTarget: richness.mechanicsTarget,
        enrichModel: richness.enrichModel || undefined,
        extraMechanics,
        seed,
        // 議論適性ゲート (09): 情報ゲートの後段・人間レビューの前 (無効時は undefined = 現行挙動)。
        debatability: resolveDebatabilityGate({ kind, sessionId, llm: webDeps.llm }),
        warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
      });
      info.fixSuggestions = await suggestFixesForPaper({
        draft,
        info,
        llm: webDeps.llm,
        model: getConfig().flow.paperReview.model || undefined,
        warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
      });
      const entry = paperReviews.get(sessionId);
      if (entry) {
        Object.assign(entry, { ready: true, draft, info });
        // 初期草案を版履歴の rev1 として記録 (Notion 風編集の「戻す」基点)。
        appendRevision({ sessionId, bodyMd: draft.bodyMd, changeSummary: "初期草案", origin: "initial" });
        // ドラフトを discussion_paper(status='draft') に永続 → 議論一覧に「下書き」として出す/再開できる。
        persistDraftPaper(
          { sessionId, theme: draft.theme, tags: draft.tags, mechanics: draft.mechanics, supplement: draft.supplement, bodyMd: draft.bodyMd },
          kind
        );
        savePaperReviewInfo(sessionId, info);
        scheduleWebAutoApprove(sessionId, webDeps); // 無操作タイムアウト (timeoutMs>0 時のみ)
      }
    })().catch((e) => {
      const entry = paperReviews.get(sessionId);
      if (entry) Object.assign(entry, { ready: true, error: (e as Error).message });
      console.warn(`[flow-web] ${kind} ペーパー草案エラー: ${(e as Error).message}`);
    });
    return c.json({ ok: true, kind, sessionId, review: true });
  }

  void (async () => {
    await prepareInformationBeforeFlow(kind, theme, tags, sessionId, autoCrawlSpec, webDeps);
    await dispatchFlow({ theme, tags, flow: kind, rounds, turnsPerRound }, { ...dispatchDeps, sessionId });
  })()
    .catch((e) => console.warn(`[flow-web] ${kind} 実行エラー: ${(e as Error).message}`))
    .finally(() => finished.add(sessionId));
  return c.json({ ok: true, kind, sessionId });
});

/** ドラフト + ブロック分解 + 版履歴状態を 1 つの JSON にまとめる (Notion 風 UI 用)。 */
function paperPayload(sessionId: string, draft: PaperDraft, entry?: WebPaperReview) {
  return {
    paper: draft,
    info: entry?.info ?? storedPaperReviewInfo(sessionId) ?? null,
    fixedFields: paperFixedFieldsFromMarkdown(draft.bodyMd, draft),
    settings: {
      rounds: entry?.rounds ?? null,
      turnsPerRound: entry?.turnsPerRound ?? null,
    },
    blocks: splitBlocks(draft.bodyMd),
    canRevert: canRevert(sessionId),
    rev: latestRevisionRev(sessionId),
  };
}

/** 最新リビジョン番号 (無ければ 0)。 */
function latestRevisionRev(sessionId: string): number {
  const rows = listRevisionsSafe(sessionId);
  return rows.length ? rows[rows.length - 1].rev : 0;
}
function listRevisionsSafe(sessionId: string): Array<{ rev: number }> {
  try {
    return listRevisions(sessionId);
  } catch {
    return [];
  }
}

/** ドラフトを本文 md で更新し、版履歴に追記して自動開始タイマーを延長する。 */
function commitBodyMd(
  sessionId: string,
  entry: WebPaperReview,
  bodyMd: string,
  changeSummary: string,
  origin: "llm-edit" | "crawl" | "manual",
  webDeps: FlowWebDeps
): PaperDraft {
  const draft = withDerivedStructure(bodyMd, entry.draft as PaperDraft);
  entry.draft = draft;
  appendRevision({ sessionId, bodyMd: draft.bodyMd, changeSummary, origin });
  // ドラフト行 (一覧の「下書き」) を最新内容に同期する (議題編集で一覧の表題も追従)。
  persistDraftPaper({ sessionId, theme: draft.theme, tags: draft.tags, mechanics: draft.mechanics, supplement: draft.supplement, bodyMd: draft.bodyMd }, entry.flow);
  scheduleWebAutoApprove(sessionId, webDeps); // 編集があったら自動開始を延長
  return draft;
}

/** メモリに編集エントリが無い時、永続済みドラフト (status='draft') から復元する (再起動/再訪で編集を再開)。 */
function rehydrateDraftEntry(sessionId: string): WebPaperReview | null {
  const row = getDraftPaper(sessionId);
  if (!row) return null;
  const draft = withDerivedStructure(row.bodyMd, {
    theme: row.theme,
    tags: row.tags,
    supplement: row.supplement,
    mechanics: row.mechanics,
  });
  const entry: WebPaperReview = {
    flow: row.flow as FlowKind,
    ready: true,
    draft,
    info: storedPaperReviewInfo(sessionId),
  };
  paperReviews.set(sessionId, entry);
  return entry;
}

/** ペーパー編集草案の取得 (ready になるまでブラウザがポーリング)。 */
flowRoutes.get("/api/flow/:session/paper", (c) => {
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId) ?? rehydrateDraftEntry(sessionId);
  if (!entry) {
    const persisted = getPaperSnapshot(sessionId);
    if (persisted) {
      return c.json({
        ok: true,
        ready: false,
        started: persisted.status !== "draft",
        status: persisted.status,
        error: null,
        info: storedPaperReviewInfo(sessionId) ?? null,
        paper: null,
        blocks: [],
        paperMd: persisted.bodyMd,
      });
    }
    return c.json({ ok: true, ready: false, missing: true, error: null, info: null, paper: null, blocks: [] });
  }
  return c.json({
    ok: true,
    ready: entry.ready,
    started: false,
    missing: false,
    error: entry.error ?? null,
    ...(entry.ready && entry.draft
      ? paperPayload(sessionId, entry.draft, entry)
      : { info: entry.info ?? null, paper: null, blocks: [] }),
  });
});

/** 固定フォームの内容をペーパー本文に反映する。タグ/進行量も下書き側へ保存する。 */
flowRoutes.post("/api/flow/:session/paper/form/apply", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>;
  const current = paperFixedFieldsFromMarkdown(entry.draft.bodyMd, entry.draft);
  const nextFields: PaperFixedFields = {
    gameTitle: stringField(body, "gameTitle") || current.gameTitle,
    discussionTheme: stringField(body, "discussionTheme") || current.discussionTheme,
    discussionContent: "discussionContent" in body ? stringField(body, "discussionContent") : current.discussionContent,
    mechanicsContext: "mechanicsContext" in body ? stringField(body, "mechanicsContext") : current.mechanicsContext,
    themeSupplement: "themeSupplement" in body ? stringField(body, "themeSupplement") : current.themeSupplement,
  };
  if (!nextFields.gameTitle || !nextFields.discussionTheme) {
    return c.json({ ok: false, error: "ゲームタイトル(または主目的)と議論したいテーマは必須です" }, 400);
  }
  const tags = "tags" in body ? sanitizeReviewTags(body.tags, entry.draft.tags) : entry.draft.tags;
  entry.rounds = "rounds" in body ? readOptionalInt(body.rounds, 1, 10) : entry.rounds;
  entry.turnsPerRound = "turnsPerRound" in body ? readOptionalInt(body.turnsPerRound, 1, 20) : entry.turnsPerRound;
  // 論点 (09): 1 行 1 論点のテキスト or 配列で受ける (フィールド未指定なら現行値を保持)。
  const issues = "issues" in body ? parseIssuesField(body.issues) : entry.draft.issues;

  const bodyMd = paperDraftToMarkdown({
    ...entry.draft,
    ...nextFields,
    theme: nextFields.discussionTheme,
    tags,
    supplement: nextFields.themeSupplement,
    mechanics: entry.draft.mechanics,
    issues,
  });
  commitBodyMd(sessionId, entry, bodyMd, "固定フォーム保存", "manual", webDeps);
  return c.json({ ok: true, ...paperPayload(sessionId, entry.draft, entry) });
});

/** 保存済みドラフトを対象に、議論可能性を明示チェックする。結果は下書きに永続して常時表示する。 */
flowRoutes.post("/api/flow/:session/paper/debatability/check", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId) ?? rehydrateDraftEntry(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  const gate = manualDebatabilityGate(entry.flow, sessionId, webDeps.llm);
  if (!gate) return c.json({ ok: false, error: "このフローでは議論可能性チェックを実行できません" }, 400);

  const voices = webDeps.listExternalVoices
    ? webDeps.listExternalVoices([entry.draft.theme], DEBATABILITY_VOICE_LIMIT)
    : [];
  const result = await assessDebatability({
    theme: entry.draft.theme,
    paperMd: entry.draft.bodyMd,
    voices,
    llm: gate.llm,
    minArmableIssues: gate.minArmableIssues,
    warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
  });

  const info: PaperReviewInfo = {
    ...defaultPaperInfo(entry.info),
    voiceCount: voices.length,
    countCapped: voices.length >= DEBATABILITY_VOICE_LIMIT,
    samples: reviewInfoSamples(voices),
    debatability: result,
  };
  info.fixSuggestions = await suggestFixesForPaper({
    draft: entry.draft,
    info,
    llm: webDeps.llm,
    model: getConfig().flow.paperReview.model || undefined,
    warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
  });
  entry.info = info;
  savePaperReviewInfo(sessionId, info);

  const issues = annotatedIssues(result);
  if (issues.length > 0) {
    const nextMd = paperDraftToMarkdown({ ...entry.draft, issues });
    commitBodyMd(sessionId, entry, nextMd, "議論可能性チェック", "manual", webDeps);
  }

  return c.json({ ok: true, ...paperPayload(sessionId, entry.draft, entry) });
});

/** LLM が持っているゲームメカニクス理解度を確認し、結果を保存する。 */
flowRoutes.post("/api/flow/:session/paper/mechanics/check", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId) ?? rehydrateDraftEntry(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  const fields = paperFixedFieldsFromMarkdown(entry.draft.bodyMd, entry.draft);
  const info: PaperReviewInfo = {
    ...defaultPaperInfo(entry.info),
    mechanicsKnowledge: await checkMechanicsKnowledge({
      draft: entry.draft,
      fields,
      llm: webDeps.llm,
      model: getConfig().flow.paperReview.model || undefined,
      warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
    }),
  };
  entry.info = info;
  savePaperReviewInfo(sessionId, info);
  return c.json({ ok: true, ...paperPayload(sessionId, entry.draft, entry) });
});

/** 旧「全体調整」は任意指示を LLM に渡すため廃止。チェック系の固定パターンだけを使う。 */
flowRoutes.post("/api/flow/:session/paper/edit", (c) =>
  c.json(
    {
      ok: false,
      error: "全体調整は廃止しました。議論可能か確認する、メカニクス知識を確認、追加学習を使ってください。",
    },
    410
  )
);

/** 1 ブロックを LLM で改稿提案する (適用しない=UI が diff 提示して採否)。 */
flowRoutes.post("/api/flow/:session/paper/block/review", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  const body = (await c.req.json().catch(() => ({}))) as { blockId?: unknown; instruction?: unknown };
  const blockId = typeof body.blockId === "string" ? body.blockId : "";
  if (!blockId) return c.json({ ok: false, error: "blockId が必要です" }, 400);
  const result = await reviewBlock({
    bodyMd: entry.draft.bodyMd,
    blockId,
    instruction: typeof body.instruction === "string" ? body.instruction : undefined,
    llm: deps.llm,
    model: getConfig().flow.paperReview.model || undefined,
    warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
  });
  // result.ok (改稿成否) は envelope の ok と衝突するので reviewed に分離する。
  const { ok: reviewed, ...rest } = result;
  return c.json({ ok: true, reviewed, ...rest });
});

/** ブロックの改稿/手編集を本文に適用する (採用 or 手動編集の確定)。 */
flowRoutes.post("/api/flow/:session/paper/block/apply", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  const body = (await c.req.json().catch(() => ({}))) as { blockId?: unknown; newText?: unknown; summary?: unknown };
  const blockId = typeof body.blockId === "string" ? body.blockId : "";
  const newText = typeof body.newText === "string" ? body.newText : "";
  if (!blockId) return c.json({ ok: false, error: "blockId が必要です" }, 400);
  const nextMd = replaceBlock(entry.draft.bodyMd, blockId, newText);
  const summary = typeof body.summary === "string" && body.summary.trim() ? body.summary.trim() : "ブロックを編集";
  commitBodyMd(sessionId, entry, nextMd, summary, "manual", webDeps);
  return c.json({ ok: true, ...paperPayload(sessionId, entry.draft, entry) });
});

/** ブロックの根拠を集めて (RAG) 挿入用段落を提案する (クロール補佐)。insert=true で本文に追記。 */
flowRoutes.post("/api/flow/:session/paper/crawl", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  if (!webDeps.listExternalVoices) return c.json({ ok: false, error: "外部の声の参照が無効です" }, 400);
  const body = (await c.req.json().catch(() => ({}))) as { blockId?: unknown; query?: unknown; insert?: unknown };
  const blockId = typeof body.blockId === "string" ? body.blockId : "";
  const query = typeof body.query === "string" && body.query.trim() ? body.query.trim() : "";
  // 検索語: 明示 query > ブロック本文 > 議題。
  const blockText = blockId ? splitBlocks(entry.draft.bodyMd).find((b) => b.id === blockId)?.text : undefined;
  const topic = query || (blockText ?? "").replace(/^#{1,6}\s+/, "").slice(0, 80) || entry.draft.theme;
  const evidence = await gatherEvidence({
    topic,
    listExternalVoices: webDeps.listExternalVoices,
    llm: webDeps.llm,
    model: getConfig().flow.paperReview.model || undefined,
    warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
  });
  // insert=true なら提案段落を対象ブロック直後 (無ければ末尾) に挿入して確定。
  if (body.insert === true && evidence.suggestion) {
    const nextMd = blockId
      ? insertBlockAfter(entry.draft.bodyMd, blockId, evidence.suggestion)
      : `${entry.draft.bodyMd}\n\n${evidence.suggestion}`;
    commitBodyMd(sessionId, entry, nextMd, `根拠を追記 (${evidence.voices.length} 件)`, "crawl", webDeps);
    return c.json({ ok: true, inserted: true, evidence, ...paperPayload(sessionId, entry.draft, entry) });
  }
  return c.json({ ok: true, inserted: false, evidence });
});

/** 1 手前の本文に戻す (版履歴 revert)。 */
flowRoutes.post("/api/flow/:session/paper/revert", (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  const reverted = revertLast(sessionId);
  if (!reverted) return c.json({ ok: false, error: "これ以上戻せません" }, 409);
  entry.draft = withDerivedStructure(reverted.bodyMd, entry.draft as PaperDraft);
  scheduleWebAutoApprove(sessionId, webDeps);
  return c.json({ ok: true, changeSummary: reverted.changeSummary, ...paperPayload(sessionId, entry.draft, entry) });
});

/** ペーパーを承認して議論を開始する (body に編集後ペーパーがあればそれを確定値にする)。 */
flowRoutes.post("/api/flow/:session/paper/approve", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "レビュー準備中です" }, 409);
  const body = (await c.req.json().catch(() => ({}))) as { paper?: unknown };
  // body.paper があれば Web フォームの直接編集を確定値に採用 (なければ蓄積済みドラフト)。
  const finalPaper = coercePaperDraft(body.paper, entry.draft);
  startApprovedFlow(sessionId, entry, finalPaper, webDeps);
  return c.json({ ok: true, sessionId });
});

flowRoutes.post("/api/flow/:session/say", async (c) => {
  const sessionId = c.req.param("session");
  const session = sparringSessions.get(sessionId);
  if (!session) return c.json({ ok: false, error: "セッションが見つかりません" }, 404);
  const body = (await c.req.json().catch(() => ({}))) as { text?: unknown };
  const text = typeof body.text === "string" ? body.text.trim() : "";
  if (!text) return c.json({ ok: false, error: "発話が空です" }, 400);
  const result = await session.submitUser(text);
  if (result.kind === "ended") finished.add(sessionId);
  return c.json({ ok: true, result: { kind: result.kind } });
});

/** 議論一覧の絞り込み state を query から解決する (不正/未指定は 'all')。 */
function parseSessionState(v: string | undefined): FlowSessionState {
  return v === "draft" || v === "live" || v === "concluded" ? v : "all";
}
/** 議論一覧の参照範囲。all=共通一覧、ai=AI議論、chat=チャット/壁打ち。 */
function parseSessionScope(v: string | undefined): FlowSessionScope {
  return v === "ai" || v === "chat" ? v : "all";
}
/** 整数 query を範囲クランプして読む (不正/未指定は fallback)。 */
function clampIntQuery(v: string | undefined, fallback: number, min: number, max: number): number {
  const n = Number.parseInt(v ?? "", 10);
  if (!Number.isFinite(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}

/**
 * 議論一覧 (開始済みの discussion_paper を新しい順)。進行中/収束済みを問わず在庫を返す。
 * 絞り込み `?state=draft|live|concluded` (既定 all) + ページング `?limit=&offset=` (既定 100/0)。
 * total/hasMore を返し、UI が「もっと見る」を出せる。
 */
flowRoutes.get("/api/flow/sessions", (c) => {
  const state = parseSessionState(c.req.query("state"));
  const scope = parseSessionScope(c.req.query("scope"));
  const limit = clampIntQuery(c.req.query("limit"), 100, 1, 200);
  const offset = clampIntQuery(c.req.query("offset"), 0, 0, Number.MAX_SAFE_INTEGER);
  const { rows, total } = listFlowSessions({ state, scope, limit, offset });
  return c.json({
    ok: true,
    total,
    limit,
    offset,
    scope,
    hasMore: offset + rows.length < total,
    sessions: rows.map((r) => {
      const concluded = r.concluded === 1;
      // 表示状態: draft (編集中・未確定) / concluded (結論あり) / live (進行/未収束)。
      const st = r.status === "draft" ? "draft" : concluded ? "concluded" : "live";
      return { ...r, concluded, state: st };
    }),
  });
});

/** 議論 (下書き/進行中/収束済みいずれも) を削除する。永続行 + 進行中のインメモリ状態を破棄。 */
flowRoutes.delete("/api/flow/:session", (c) => {
  const sessionId = c.req.param("session");
  // 進行中セッションのインメモリ状態を先に片付ける (タイマー/壁打ち/完了フラグ)。
  const review = paperReviews.get(sessionId);
  if (review?.timer) clearTimeout(review.timer);
  paperReviews.delete(sessionId);
  sparringSessions.delete(sessionId);
  finished.delete(sessionId);
  const removed = deleteFlowSession(sessionId);
  if (!removed) return c.json({ ok: false, error: "対象の議論が見つかりません" }, 404);
  return c.json({ ok: true, sessionId });
});

flowRoutes.get("/api/flow/:session/status", (c) => {
  const sessionId = c.req.param("session");
  const since = Number.parseInt(c.req.query("since") ?? "0", 10) || 0;
  const db = getFlowDb();

  const utterances = db
    .prepare(
      `SELECT id, round, turn, persona_name, role, stance, possession_name, text, is_error, created_at
         FROM flow_utterance
        WHERE session_id = ? AND created_at > ?
        ORDER BY created_at ASC`
    )
    .all(sessionId, since) as Array<{
    id: string;
    round: number;
    turn: number;
    persona_name: string;
    role: string;
    stance: string;
    possession_name: string | null;
    text: string;
    is_error: number;
    created_at: number;
  }>;

  const voteRows = db
    .prepare(
      `SELECT round, chosen_utterance_id AS id, COUNT(*) AS votes
         FROM vote
        WHERE session_id = ? AND chosen_utterance_id IS NOT NULL
        GROUP BY round, chosen_utterance_id
        ORDER BY round ASC`
    )
    .all(sessionId) as Array<{ round: number; id: string; votes: number }>;
  const voteById = new Map<string, number>();
  const voteWinnerByRound = new Map<number, { id: string; votes: number }>();
  for (const r of voteRows) {
    voteById.set(r.id, (voteById.get(r.id) ?? 0) + r.votes);
    const cur = voteWinnerByRound.get(r.round);
    if (!cur || r.votes > cur.votes) voteWinnerByRound.set(r.round, { id: r.id, votes: r.votes });
  }
  const scoreWinnerRows = db
    .prepare(
      `SELECT round, utterance_id AS id
         FROM improvement_score
        WHERE session_id = ? AND is_winner = 1`
    )
    .all(sessionId) as Array<{ round: number; id: string }>;
  const topRow = db
    .prepare(`SELECT top_utterance_ids_json FROM flow_conclusion WHERE session_id = ?`)
    .get(sessionId) as { top_utterance_ids_json: string } | undefined;
  let topIds: string[] = [];
  try {
    const parsed = topRow ? JSON.parse(topRow.top_utterance_ids_json) : [];
    if (Array.isArray(parsed)) topIds = parsed.filter((id): id is string => typeof id === "string");
  } catch {
    topIds = [];
  }
  const winnerIds = new Set(topIds);
  for (const w of voteWinnerByRound.values()) if (w.votes > 0) winnerIds.add(w.id);
  for (const w of scoreWinnerRows) winnerIds.add(w.id);
  const roundRows = db
    .prepare(`SELECT round, aufhebung_json FROM discussion_paper_round WHERE paper_id IN (SELECT id FROM discussion_paper WHERE session_id = ?)`)
    .all(sessionId) as Array<{ round: number; aufhebung_json: string }>;
  const aufhebungByRound = new Map(
    roundRows.map((r) => {
      let values: string[] = [];
      try {
        const parsed = JSON.parse(r.aufhebung_json);
        if (Array.isArray(parsed)) values = parsed.filter((v): v is string => typeof v === "string");
      } catch {
        values = [];
      }
      return [r.round, values] as const;
    })
  );
  const markRows = db
    .prepare(`SELECT id, round FROM flow_utterance WHERE session_id = ?`)
    .all(sessionId) as Array<{ id: string; round: number }>;
  const markFieldsFor = (id: string, round: number) => ({
    votes: voteById.get(id) ?? 0,
    isWinner: winnerIds.has(id),
    // 止揚はラウンド全体ではなく、世論として使われた意見だけに付ける。
    roundAufhebung: winnerIds.has(id) ? (aufhebungByRound.get(round) ?? []) : [],
  });

  const conclusionRow = db
    .prepare(`SELECT summary, concluded FROM flow_conclusion WHERE session_id = ?`)
    .get(sessionId) as { summary: string; concluded: number } | undefined;

  const session = sparringSessions.get(sessionId);
  const done = finished.has(sessionId) || (session?.isEnded ?? false) || !!conclusionRow;

  return c.json({
    ok: true,
    utterances: utterances.map((u) => ({
      id: u.id,
      round: u.round,
      turn: u.turn,
      personaName: u.persona_name,
      // 「名前 (ロール/憑依ペルソナ)」(item2/4)。憑依なしは括弧内ロールのみ。
      displayName: composeDisplayName({
        name: u.persona_name,
        stance: u.stance as FlowStance,
        role: u.role as FlowRole,
        possessionName: u.possession_name,
      }),
      role: u.role,
      stance: u.stance,
      possessionName: u.possession_name,
      text: u.text,
      isError: u.is_error === 1,
      ...markFieldsFor(u.id, u.round),
      createdAt: u.created_at,
    })),
    marks: markRows
      .map((u) => ({ id: u.id, ...markFieldsFor(u.id, u.round) }))
      .filter((m) => m.votes > 0 || m.isWinner || m.roundAufhebung.length > 0),
    conclusion: conclusionRow?.summary ?? null,
    concluded: conclusionRow?.concluded === 1,
    // ライブのディスカッションペーパー本文 (議論進行で更新されていく)。
    paperMd: getPaperBodyBySession(sessionId),
    done,
  });
});

/** テスト用: 登録セッションをクリアする。 */
export function _resetFlowWeb(): void {
  sparringSessions.clear();
  finished.clear();
  for (const r of paperReviews.values()) if (r.timer) clearTimeout(r.timer);
  paperReviews.clear();
}
