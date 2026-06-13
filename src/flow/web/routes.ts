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
import { dispatchFlow, parseFlowKind, type DispatchDeps } from "../dispatch.js";
import type { SparringSession } from "../sparring.js";
import type { FlowTag } from "../tags.js";
import { FLOW_HTML } from "./page.js";

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
  const body = (await c.req.json().catch(() => ({}))) as {
    theme?: unknown;
    flow?: unknown;
    tags?: unknown;
  };
  const theme = typeof body.theme === "string" ? body.theme.trim() : "";
  const flowLabel = typeof body.flow === "string" ? body.flow : "";
  const tags = Array.isArray(body.tags)
    ? (body.tags.filter((t) => typeof t === "string") as FlowTag[])
    : [];

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
    const result = await dispatchFlow({ theme, tags, flow: kind, scene: `web:flow-${randomUUID().slice(0, 8)}` }, dispatchDeps);
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

  // 議論 / 改善: sessionId を先に発番し、バックグラウンドで完走させてポーリングで追う
  const sessionId = randomUUID();
  void dispatchFlow({ theme, tags, flow: kind }, { ...dispatchDeps, sessionId })
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
      `SELECT persona_name, role, text, is_error, created_at
         FROM flow_utterance
        WHERE session_id = ? AND created_at > ?
        ORDER BY created_at ASC`
    )
    .all(sessionId, since) as Array<{
    persona_name: string;
    role: string;
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
      role: u.role,
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
