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
import { getPaperBodyBySession, persistDraftPaper, getDraftPaper } from "../discussion-paper.js";

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
  reviewBlock,
  gatherEvidence,
  withDerivedStructure,
  type PaperDraft,
  type PaperReviewInfo,
} from "../paper-review.js";
import { splitBlocks, replaceBlock, insertBlockAfter } from "../paper-blocks.js";
import { appendRevision, canRevert, revertLast, listRevisions } from "../paper-revisions.js";
import { getConfig } from "../../config.js";
import { FLOW_HTML } from "./page.js";

/** Web 議論/改善をペーパー編集ゲート経由にするか (enabled か webCanonical のどちらか)。 */
function webPaperGateEnabled(): boolean {
  const pr = getConfig().flow.paperReview;
  return pr.enabled || pr.webCanonical;
}

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
    const youtubeApiKey = await resolveYoutubeApiKey(d);
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
      youtubeApiKey,
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
    const youtubeApiKey = await resolveYoutubeApiKey(d);
    const result = await ensureLearningData({
      core,
      theme,
      slug: deriveSlug(theme),
      workspaceId: d.workspaceId,
      spec,
      minVoices: cfg.minVoices,
      maxItems: cfg.maxItems,
      listExternalVoices: d.listExternalVoices,
      youtubeApiKey: youtubeApiKey ?? undefined,
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
  getYoutubeApiKey?: () => Promise<string | null>;
}

let deps: FlowWebDeps | null = null;

export function setFlowWebDeps(d: FlowWebDeps): void {
  deps = d;
}

async function resolveYoutubeApiKey(d: FlowWebDeps): Promise<string | null> {
  return d.getYoutubeApiKey ? await d.getYoutubeApiKey() : d.youtubeApiKey ?? null;
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
    {
      ...dispatchDeps,
      sessionId,
      paperOverride: {
        mechanics: finalPaper.mechanics,
        supplement: finalPaper.supplement,
        bodyMd: finalPaper.bodyMd,
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
    // youtube API キーは runtime 解決 (tuning UI / gcloud secret 経由)。
    const youtubeApiKey = await resolveYoutubeApiKey(webDeps);
    const learningCrawl = buildLearningCrawl({ ...body, youtubeApiKey });
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

  // ペーパー編集ゲート (Web 正規フロー): 情報整備後に草案を作り、ブラウザの編集/承認を待つ。
  if (webPaperGateEnabled()) {
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
        // 初期草案を版履歴の rev1 として記録 (Notion 風編集の「戻す」基点)。
        appendRevision({ sessionId, bodyMd: draft.bodyMd, changeSummary: "初期草案", origin: "initial" });
        // ドラフトを discussion_paper(status='draft') に永続 → 議論一覧に「下書き」として出す/再開できる。
        persistDraftPaper({ sessionId, theme, tags: draft.tags, mechanics: draft.mechanics, supplement: draft.supplement, bodyMd: draft.bodyMd }, kind);
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
function paperPayload(sessionId: string, draft: PaperDraft) {
  return {
    paper: draft,
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
  const entry: WebPaperReview = { flow: row.flow as FlowKind, ready: true, draft };
  paperReviews.set(sessionId, entry);
  return entry;
}

/** ペーパー編集草案の取得 (ready になるまでブラウザがポーリング)。 */
flowRoutes.get("/api/flow/:session/paper", (c) => {
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId) ?? rehydrateDraftEntry(sessionId);
  if (!entry) return c.json({ ok: false, error: "編集対象が見つかりません" }, 404);
  return c.json({
    ok: true,
    ready: entry.ready,
    error: entry.error ?? null,
    info: entry.info ?? null,
    ...(entry.ready && entry.draft ? paperPayload(sessionId, entry.draft) : { paper: null, blocks: [] }),
  });
});

/** 自然文の調整指示をペーパー全体に反映する (Web の NL 編集・本文 md 再生成)。 */
flowRoutes.post("/api/flow/:session/paper/edit", async (c) => {
  if (!deps) return c.json({ ok: false, error: "flow web 未初期化" }, 500);
  const webDeps = deps;
  const sessionId = c.req.param("session");
  const entry = paperReviews.get(sessionId);
  if (!entry || !entry.ready || !entry.draft) return c.json({ ok: false, error: "編集準備中です" }, 409);
  const body = (await c.req.json().catch(() => ({}))) as { instruction?: unknown };
  const instruction = typeof body.instruction === "string" ? body.instruction.trim() : "";
  if (!instruction) return c.json({ ok: false, error: "調整指示が空です" }, 400);
  const edited = await applyPaperEdit(entry.draft, instruction, webDeps.llm, {
    model: getConfig().flow.paperReview.model || undefined,
    warn: (m) => console.warn(`[flow-web/paper ${sessionId}] ${m}`),
  });
  if (edited.applied) commitBodyMd(sessionId, entry, edited.draft.bodyMd, edited.changeSummary, "llm-edit", webDeps);
  return c.json({ ok: true, changeSummary: edited.changeSummary, applied: edited.applied, ...paperPayload(sessionId, entry.draft) });
});

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
  return c.json({ ok: true, ...paperPayload(sessionId, entry.draft) });
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
    return c.json({ ok: true, inserted: true, evidence, ...paperPayload(sessionId, entry.draft) });
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
  return c.json({ ok: true, changeSummary: reverted.changeSummary, ...paperPayload(sessionId, entry.draft) });
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

/** 議論一覧 (開始済みの discussion_paper を新しい順)。進行中/収束済みを問わず在庫を返す。 */
flowRoutes.get("/api/flow/sessions", (c) => {
  const db = getFlowDb();
  const rows = db
    .prepare(
      `SELECT dp.session_id AS sessionId, dp.theme AS theme, dp.flow AS flow, dp.status AS status,
              dp.created_at AS createdAt, dp.updated_at AS updatedAt,
              (SELECT COUNT(*) FROM flow_utterance fu WHERE fu.session_id = dp.session_id) AS utterances,
              (SELECT concluded FROM flow_conclusion fc WHERE fc.session_id = dp.session_id) AS concluded
         FROM discussion_paper dp
        ORDER BY dp.created_at DESC
        LIMIT 100`
    )
    .all() as Array<{
    sessionId: string;
    theme: string;
    flow: string;
    status: string;
    createdAt: number;
    updatedAt: number;
    utterances: number;
    concluded: number | null;
  }>;
  return c.json({
    ok: true,
    sessions: rows.map((r) => {
      const concluded = r.concluded === 1;
      // 表示状態: draft (編集中・未確定) / concluded (結論あり) / live (進行/未収束)。
      const state = r.status === "draft" ? "draft" : concluded ? "concluded" : "live";
      return { ...r, concluded, state };
    }),
  });
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
