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
import type { ContextVoice } from "../discussion-paper.js";

type Core = ReturnType<typeof createCore>;
import { getFlowDb } from "../db/connection.js";
import { dispatchFlow, parseFlowKind, type DispatchDeps, type FlowKind } from "../dispatch.js";
import type { SparringSession } from "../sparring.js";
import type { FlowTag } from "../tags.js";
import { composeDisplayName } from "../persona-display.js";
import type { FlowRole, FlowStance } from "../personas.js";
import {
  ensureLearningData,
  isAutoCrawlSource,
  resolveAutoCrawlSources,
  deriveSlug,
  type AutoCrawlSpec,
} from "../learning-autocrawl.js";
import type { LearningCrawlSpec } from "../learning.js";
import { analyzeSpecMechanics } from "../spec-analyze.js";
import { resolveSpecText } from "../spec-source.js";
import type { GameMechanicEntry } from "../games-md.js";
import { gateBeforeFlow } from "../information-gate-runner.js";
import {
  buildPaperDraft,
  applyPaperEdit,
  coercePaperDraft,
  type PaperDraft,
  type PaperReviewInfo,
} from "../paper-review.js";
import { getConfig } from "../../config.js";
import { FLOW_HTML } from "./page.js";

/** リクエストボディ + config 既定から自動クロール指定を組み立てる。 enabled=false / 不正ソースは null。 */
function buildAutoCrawlSpec(body: {
  learningSource?: unknown;
  learningQuery?: unknown;
  learningAppId?: unknown;
  learningUrls?: unknown;
}): AutoCrawlSpec | null {
  const cfg = getConfig().flow.autoCrawl;
  if (!cfg.enabled) return null;
  const requested = typeof body.learningSource === "string" ? body.learningSource.trim() : "";
  const source = requested || cfg.source;
  if (!isAutoCrawlSource(source)) return null;
  const query = typeof body.learningQuery === "string" ? body.learningQuery.trim() : undefined;
  const appId =
    typeof body.learningAppId === "number"
      ? body.learningAppId
      : typeof body.learningAppId === "string" && body.learningAppId.trim() !== ""
        ? Number(body.learningAppId)
        : undefined;
  const urls = Array.isArray(body.learningUrls)
    ? body.learningUrls.filter((u): u is string => typeof u === "string" && u.trim() !== "")
    : typeof body.learningUrls === "string" && body.learningUrls.trim() !== ""
      ? body.learningUrls.split(/[\s,]+/).filter(Boolean)
      : undefined;
  return { source, query, appId, urls };
}

/**
 * 学習フローの自動収集指定を組み立てる (① 類似ゲームの自動収集)。
 *   - UI が learningSource を明示した場合は、その単一ソース (steam/website は appId/urls 付き) を使う。
 *   - 未指定なら config.flow.autoCrawl の自動経路ソース横断 (niconico + キーがあれば youtube)。
 * 収集可能なソースが無ければ undefined (= 収集しない)。
 */
function buildLearningCrawl(body: {
  learningSource?: unknown;
  learningQuery?: unknown;
  learningAppId?: unknown;
  learningUrls?: unknown;
  youtubeApiKey?: string | null;
}): LearningCrawlSpec | undefined {
  const cfg = getConfig().flow.autoCrawl;
  if (!cfg.enabled) return undefined;
  const youtubeApiKey = body.youtubeApiKey ?? undefined;
  const requested = typeof body.learningSource === "string" ? body.learningSource.trim() : "";

  // UI 明示ソース: buildAutoCrawlSpec の解決 (appId/urls 込み) をそのまま単一ソースに倒す。
  if (requested) {
    const spec = buildAutoCrawlSpec(body);
    if (!spec) return undefined;
    return {
      sources: [spec.source],
      query: spec.query,
      maxItems: cfg.maxItems,
      youtubeApiKey,
      specBySource: { [spec.source]: { appId: spec.appId, urls: spec.urls } },
    };
  }

  // 既定: config のソースを自動経路に絞って横断収集。
  const sources = resolveAutoCrawlSources(cfg.sources, youtubeApiKey);
  if (sources.length === 0) return undefined;
  return {
    sources,
    query: typeof body.learningQuery === "string" ? body.learningQuery.trim() || undefined : undefined,
    maxItems: cfg.maxItems,
    youtubeApiKey,
  };
}

/**
 * 議論/改善の開始前に情報を整える。
 *   1. 情報ゲート (LLM が情報密度を評価し、不足観点を狙って学習 → 再評価) が有効ならそれを実行。
 *   2. ゲート対象外 (無効/フロー違い/Core 無し) なら、従来のカウント閾値 autoCrawl にフォールバック。
 * いずれも失敗は議論を止めない (graceful)。
 */
async function prepareInformationBeforeFlow(
  kind: FlowKind,
  theme: string,
  tags: readonly FlowTag[],
  sessionId: string,
  spec: AutoCrawlSpec | null,
  d: FlowWebDeps
): Promise<void> {
  // 情報ゲート優先 (LLM 評価 + 不足観点クロール)。
  try {
    const gate = await gateBeforeFlow({
      kind,
      theme,
      tags,
      llm: d.llm,
      openCore: d.openCore,
      workspaceId: d.workspaceId,
      listExternalVoices: d.listExternalVoices,
      sessionId,
      log: (m) => console.log(`[flow-gate] ${m}`),
      warn: (m) => console.warn(`[flow-gate] ${m}`),
      youtubeApiKey: d.youtubeApiKey,
    });
    if (gate) return; // ゲートが学習まで担った
  } catch (e) {
    console.warn(`[flow-gate] 情報ゲート失敗 (議論は続行): ${(e as Error).message}`);
  }
  // フォールバック: 従来のカウント閾値 autoCrawl。
  await legacyAutoCrawlBeforeFlow(kind, theme, spec, d);
}

/**
 * 旧来の自動クロール (テーマの外部の声が minVoices 件未満なら指定ソースでクロール → 取込)。
 * core 未設定 / 自動クロール無効ならスキップ。クロール失敗は議論を止めない (graceful)。
 */
async function legacyAutoCrawlBeforeFlow(
  kind: FlowKind,
  theme: string,
  spec: AutoCrawlSpec | null,
  d: FlowWebDeps
): Promise<void> {
  if (!spec || !d.openCore || (kind !== "discussion" && kind !== "improvement")) return;
  const cfg = getConfig().flow.autoCrawl;
  const core = d.openCore();
  try {
    const result = await ensureLearningData({
      core,
      theme,
      slug: deriveSlug(theme),
      workspaceId: d.workspaceId,
      spec,
      minVoices: cfg.minVoices,
      maxItems: cfg.maxItems,
      listExternalVoices: d.listExternalVoices,
      youtubeApiKey: d.youtubeApiKey ?? undefined,
      log: (m) => console.log(`[flow-autocrawl] ${m}`),
      warn: (m) => console.warn(`[flow-autocrawl] ${m}`),
    });
    if (!result.skipped) console.log(`[flow-autocrawl] ${result.message}`);
  } catch (e) {
    console.warn(`[flow-autocrawl] クロール失敗 (議論は続行): ${(e as Error).message}`);
  } finally {
    core.close?.();
  }
}

export interface FlowWebDeps {
  workspaceId: string;
  /** フロー実行用 LLM クライアント。 */
  llm: LLMClient;
  /** Discatier Core を開くファクトリ (learning に必要)。 */
  openCore?: () => Core;
  listExternalVoices?: (terms: string[], limit: number) => ContextVoice[];
  sentimentClients?: CascadeClients;
  gamesDir?: string;
  youtubeApiKey?: string | null;
}

let deps: FlowWebDeps | null = null;

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
 * 調整 (/paper/edit or 直接編集) → /paper/approve で議論を開始する。
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
  void dispatchFlow(
    { theme: finalPaper.theme, tags: finalPaper.tags, flow: entry.flow, rounds: entry.rounds, turnsPerRound: entry.turnsPerRound },
    { ...dispatchDeps, sessionId, paperOverride: { mechanics: finalPaper.mechanics, supplement: finalPaper.supplement } }
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

export const flowRoutes = new Hono();

flowRoutes.get("/flow", (c) => c.html(FLOW_HTML));

flowRoutes.post("/api/flow/start", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps; // module-level let を closure で使うため const に束ねる
  const body = (await c.req.json().catch(() => ({}))) as {
    theme?: unknown;
    flow?: unknown;
    tags?: unknown;
    rounds?: unknown;
    turnsPerRound?: unknown;
    opponent?: unknown;
    learningSource?: unknown;
    learningQuery?: unknown;
    learningAppId?: unknown;
    learningUrls?: unknown;
    specText?: unknown;
    specUrl?: unknown;
  };
  const theme = typeof body.theme === "string" ? body.theme.trim() : "";
  const flowLabel = typeof body.flow === "string" ? body.flow : "";
  const tags = Array.isArray(body.tags)
    ? (body.tags.filter((t) => typeof t === "string") as FlowTag[])
    : [];
  // 議論ごとのラウンド/ターン数 (任意)。runFlow が config 既定にフォールバック + 上限クランプする。
  const toNum = (v: unknown): number | undefined =>
    typeof v === "number" ? v : typeof v === "string" && v.trim() !== "" ? Number(v) : undefined;
  const rounds = toNum(body.rounds);
  const turnsPerRound = toNum(body.turnsPerRound);
  // G: 壁打ち相手 (カンマ区切りの名前/ID)。sparring のみ反映。
  const opponentPersonaIds =
    typeof body.opponent === "string" && body.opponent.trim() !== ""
      ? body.opponent.split(",").map((s) => s.trim()).filter(Boolean)
      : undefined;

  if (!theme) return c.json({ ok: false, error: "テーマは必須です" }, 400);
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
    const learningCrawl = buildLearningCrawl({ ...body, youtubeApiKey: deps.youtubeApiKey });
    const inlineSpec = typeof body.specText === "string" ? body.specText.trim() : "";
    const specUrl = typeof body.specUrl === "string" ? body.specUrl.trim() : "";
    // URL / ローカルパスから取得した仕様書を貼付本文と結合する (取得失敗は学習を止めない)。
    let specText = inlineSpec;
    if (specUrl) {
      try {
        const fetched = await resolveSpecText(specUrl, { allowLocalPath: true });
        specText = specText ? `${specText}\n\n${fetched}` : fetched;
      } catch (e) {
        console.warn(`[flow-web/spec] 仕様書ソース取得失敗 (${specUrl}): ${(e as Error).message}`);
      }
    }
    const core = deps.openCore();
    try {
      let mechanics: GameMechanicEntry[] | undefined;
      if (specText) {
        mechanics = await analyzeSpecMechanics({
          theme,
          specText,
          llm: webDeps.llm,
          warn: (m) => console.warn(`[flow-web/spec] ${m}`),
        });
      }
      const result = await dispatchFlow(
        { theme, tags, flow: kind },
        { ...dispatchDeps, core, learningCrawl, mechanics }
      );
      if (result.kind !== "learning") return c.json({ ok: false, error: "internal" }, 500);
      return c.json({ ok: true, kind, sessionId: result.result.gameSlug, result: result.result });
    } finally {
      core.close?.();
    }
  }

  // 議論 / 改善: sessionId を先に発番し、バックグラウンドで完走させてポーリングで追う。
  // 学習データが不足していれば、議論を始める前に指定ソースでクロール → 取込する (事前学習の UI 化)。
  const sessionId = randomUUID();
  const autoCrawlSpec = buildAutoCrawlSpec(body);

  // ペーパーレビューゲート (有効時): 情報整備後に草案を作り、ブラウザの調整/承認を待つ。
  if (getConfig().flow.paperReview.enabled) {
    paperReviews.set(sessionId, { flow: kind, rounds, turnsPerRound, ready: false });
    void (async () => {
      await prepareInformationBeforeFlow(kind, theme, tags, sessionId, autoCrawlSpec, webDeps);
      const richness = getConfig().flow.paperRichness;
      const { draft, info } = await buildPaperDraft(theme, tags, {
        gamesDir: webDeps.gamesDir,
        listExternalVoices: webDeps.listExternalVoices,
        llm: richness.enrichMechanics ? webDeps.llm : undefined,
        mechanicsTarget: richness.mechanicsTarget,
        enrichModel: richness.enrichModel || undefined,
        warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
      });
      const entry = paperReviews.get(sessionId);
      if (entry) {
        Object.assign(entry, { ready: true, draft, info });
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

/** ペーパーレビュー草案の取得 (ready になるまでブラウザがポーリング)。 */
flowRoutes.get("/api/flow/:session/paper", (c) => {
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry) return c.json({ ok: false, error: "レビュー対象が見つかりません" }, 404);
  return c.json({
    ok: true,
    ready: entry.ready,
    error: entry.error ?? null,
    paper: entry.draft ?? null,
    info: entry.info ?? null,
  });
});

/** 自然文の調整指示をペーパーに反映する (Web の NL 編集)。 */
flowRoutes.post("/api/flow/:session/paper/edit", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "レビュー準備中です" }, 409);
  const body = (await c.req.json().catch(() => ({}))) as { instruction?: unknown };
  const instruction = typeof body.instruction === "string" ? body.instruction.trim() : "";
  if (!instruction) return c.json({ ok: false, error: "調整指示が空です" }, 400);
  const edited = await applyPaperEdit(entry.draft, instruction, deps.llm, {
    model: getConfig().flow.paperReview.model || undefined,
    warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
  });
  entry.draft = edited.draft;
  scheduleWebAutoApprove(sessionId, deps); // 調整があったら自動開始を延長
  return c.json({ ok: true, paper: edited.draft, changeSummary: edited.changeSummary, applied: edited.applied });
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

flowRoutes.get("/api/flow/:session/status", (c) => {
  const sessionId = c.req.param("session");
  const since = Number.parseInt(c.req.query("since") ?? "0", 10) || 0;
  const db = getFlowDb();

  const utterances = db
    .prepare(
      `SELECT persona_name, role, stance, possession_name, text, is_error, created_at
         FROM flow_utterance
        WHERE session_id = ? AND created_at > ?
        ORDER BY created_at ASC`
    )
    .all(sessionId, since) as Array<{
    persona_name: string;
    role: string;
    stance: string;
    possession_name: string | null;
    text: string;
    is_error: number;
    created_at: number;
  }>;

  const conclusionRow = db
    .prepare(`SELECT summary, concluded FROM flow_conclusion WHERE session_id = ?`)
    .get(sessionId) as { summary: string; concluded: number } | undefined;

  const session = sparringSessions.get(sessionId);
  const done = finished.has(sessionId) || (session?.isEnded ?? false) || !!conclusionRow;

  return c.json({
    ok: true,
    utterances: utterances.map((u) => ({
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
      createdAt: u.created_at,
    })),
    conclusion: conclusionRow?.summary ?? null,
    concluded: conclusionRow?.concluded === 1,
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
