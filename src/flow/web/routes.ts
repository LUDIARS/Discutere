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
  deriveSlug,
  type AutoCrawlSpec,
} from "../learning-autocrawl.js";
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
 * 議論/改善の開始前に学習データが不足していれば指定ソースでクロール → 取込する。
 * core 未設定 / 自動クロール無効ならスキップ。クロール失敗は議論を止めない (graceful)。
 */
async function autoCrawlBeforeFlow(
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
      youtubeApiKey: process.env.DISCUTERE_YOUTUBE_API_KEY,
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
}

let deps: FlowWebDeps | null = null;

export function setFlowWebDeps(d: FlowWebDeps): void {
  deps = d;
}

/** flow=壁打ち の起動済みセッションを保持する (HTTP の後続発話を受けるため)。 */
const sparringSessions = new Map<string, SparringSession>();
/** discussion/improvement の完了状態 (status の done 判定用)。 */
const finished = new Set<string>();

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

  // 学習: 短時間で完走するため await して結果を返す
  if (kind === "learning") {
    if (!deps.openCore) return c.json({ ok: false, error: "learning は Core 未設定のため不可" }, 400);
    const core = deps.openCore();
    try {
      const result = await dispatchFlow({ theme, tags, flow: kind }, { ...dispatchDeps, core });
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
  void (async () => {
    await autoCrawlBeforeFlow(kind, theme, autoCrawlSpec, webDeps);
    await dispatchFlow({ theme, tags, flow: kind, rounds, turnsPerRound }, { ...dispatchDeps, sessionId });
  })()
    .catch((e) => console.warn(`[flow-web] ${kind} 実行エラー: ${(e as Error).message}`))
    .finally(() => finished.add(sessionId));
  return c.json({ ok: true, kind, sessionId });
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
}
