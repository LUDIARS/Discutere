/**
 * Facilitator — 議論の活性化と収束を司る per-discussion オーケストレータ。
 * spec/facilitator/DESIGN.md。
 *
 * 各 discussion-of-gap session を独自 tick で見張り:
 *  - 発言間隔が空いた (idle) → 新しい視点の persona を 1 体生成して投入し議論を広げる
 *  - 参加 persona が maxPersonas を超えた → 議論を要約して収束、 gap を closed に
 * gap が closed になるまで存在する (= 収束したら当該 session の管理を終える)。
 *
 * 投稿は contextProvider.postUtterance を通すので、 既存の Discord relay に乗る。
 */

import { randomUUID } from "node:crypto";

import type { createCore } from "../../core/index.js";
import type { PersonasRepo } from "../db/personas-repo.js";
import type { DiscussionContextProvider } from "../context-provider.js";
import type { LLMClient } from "../llm/client.js";
import type { Logger } from "../types.js";

import {
  buildConvergePrompt,
  buildExpandPrompt,
  extractJsonObject,
  type GeneratedPersona,
  type RecentUtterance,
} from "./prompts.js";

type Core = ReturnType<typeof createCore>;

export interface FacilitatorOptions {
  /** 見張り周期 ms (既定 30_000) */
  tickMs?: number;
  /** 発言が無くなってから「停滞」とみなす空白 ms (既定 120_000) */
  idleGapMs?: number;
  /** 参加 persona がこの数を超えたら収束へ (既定 20) */
  maxPersonas?: number;
  /** LLM model 上書き */
  model?: string;
}

export interface FacilitatorDeps {
  core: Core;
  llm: LLMClient;
  contextProvider: DiscussionContextProvider;
  personas: PersonasRepo;
  workspaceId: string;
  logger: Logger;
  options?: FacilitatorOptions;
}

interface SessionState {
  lastUtteranceCount: number;
  armed: boolean; // 次の idle で介入してよいか (発言があると再 arm)
}

interface SessionMetrics {
  utteranceCount: number;
  distinctPersonas: number;
  lastActivityAt: number;
  now: number;
}

export type FacilitatorMode = "active" | "wait" | "expand" | "converge";

/** 介入判定 (純粋関数、 テスト可能)。 */
export function evaluate(
  metrics: SessionMetrics,
  state: SessionState,
  opts: { idleGapMs: number; maxPersonas: number }
): { mode: FacilitatorMode; state: SessionState } {
  if (metrics.utteranceCount > state.lastUtteranceCount) {
    // 新しい発言があった = 活性。 カウント更新 + 次の idle に備えて再 arm。
    return { mode: "active", state: { lastUtteranceCount: metrics.utteranceCount, armed: true } };
  }
  const idleFor = metrics.now - metrics.lastActivityAt;
  if (state.armed && idleFor >= opts.idleGapMs) {
    const mode: FacilitatorMode = metrics.distinctPersonas > opts.maxPersonas ? "converge" : "expand";
    return { mode, state: { ...state, armed: false } };
  }
  return { mode: "wait", state };
}

const FACILITATOR_PERSONA_ID = "facilitator";

export interface Facilitator {
  start(): void;
  stop(): void;
  /** 1 周分を手動実行 (テスト / デバッグ) */
  tickOnce(): Promise<void>;
}

export function createFacilitator(deps: FacilitatorDeps): Facilitator {
  const tickMs = deps.options?.tickMs ?? 30_000;
  const idleGapMs = deps.options?.idleGapMs ?? 120_000;
  const maxPersonas = deps.options?.maxPersonas ?? 20;
  const raw = deps.core.client.raw;
  const states = new Map<string, SessionState>();
  let timer: NodeJS.Timeout | null = null;
  let running = false;

  // 進行役 persona を登録 (収束のまとめ発言者)
  deps.personas.insertOrIgnore({
    id: FACILITATOR_PERSONA_ID,
    name: "進行役",
    display_name: "司会 結",
    description: "議論の活性化と収束を司る中立の進行役。",
    traits: ["中立", "俯瞰", "要約", "合意形成"],
    speech_style: "落ち着いた中立的な口調。 論点を整理し、 停滞時は新しい問いを投げ、 出揃ったら要約して締める。",
  });

  interface Discussion {
    sessionId: string;
    gapId: string;
  }

  /** open な discord 議論 (discussion-of-gap session) を列挙。 */
  function listActiveDiscussions(): Discussion[] {
    const rows = raw
      .prepare(
        `SELECT s.id AS sessionId, s.title AS title
           FROM sessions s
          WHERE s.workspace_id = ?
            AND s.title LIKE 'discussion-of-gap:%'
            AND s.scene LIKE 'discord:%'`
      )
      .all(deps.workspaceId) as Array<{ sessionId: string; title: string }>;
    const out: Discussion[] = [];
    for (const r of rows) {
      const gapId = r.title.slice("discussion-of-gap:".length);
      const gap = raw
        .prepare("SELECT status FROM design_gaps WHERE id = ?")
        .get(gapId) as { status: string | null } | undefined;
      if (!gap) continue;
      if (gap.status && ["closed", "converged"].includes(gap.status)) continue;
      out.push({ sessionId: r.sessionId, gapId });
    }
    return out;
  }

  function metricsFor(sessionId: string, now: number): SessionMetrics {
    const rows = raw
      .prepare("SELECT speaker_id, posted_at FROM utterances WHERE session_id = ?")
      .all(sessionId) as Array<{ speaker_id: string | null; posted_at: number }>;
    const personaIds = new Set<string>();
    let lastActivityAt = 0;
    for (const u of rows) {
      if (u.speaker_id?.startsWith("persona:")) personaIds.add(u.speaker_id);
      if (u.posted_at > lastActivityAt) lastActivityAt = u.posted_at;
    }
    return { utteranceCount: rows.length, distinctPersonas: personaIds.size, lastActivityAt, now };
  }

  function recentUtterances(sessionId: string, limit: number): RecentUtterance[] {
    const rows = raw
      .prepare(
        "SELECT speaker_id, raw_content FROM utterances WHERE session_id = ? ORDER BY posted_at DESC LIMIT ?"
      )
      .all(sessionId, limit) as Array<{ speaker_id: string | null; raw_content: string }>;
    return rows
      .reverse()
      .map((u) => ({ speaker: speakerLabel(u.speaker_id), content: u.raw_content }));
  }

  function speakerLabel(speakerId: string | null): string {
    if (!speakerId) return "参加者";
    if (speakerId.startsWith("persona:")) {
      const p = deps.personas.get(speakerId.slice("persona:".length));
      return p?.display_name ?? speakerId.slice("persona:".length);
    }
    return "参加者";
  }

  function gapTopic(gapId: string): string {
    const g = raw
      .prepare("SELECT title, description FROM design_gaps WHERE id = ?")
      .get(gapId) as { title: string; description: string | null } | undefined;
    return g ? `${g.title}\n${g.description ?? ""}`.trim() : "(不明な議題)";
  }

  function existingPersonaNames(sessionId: string): string[] {
    const rows = raw
      .prepare(
        "SELECT DISTINCT speaker_id FROM utterances WHERE session_id = ? AND speaker_id LIKE 'persona:%'"
      )
      .all(sessionId) as Array<{ speaker_id: string }>;
    return rows.map((r) => speakerLabel(r.speaker_id));
  }

  async function expand(d: Discussion): Promise<void> {
    const { system, user } = buildExpandPrompt({
      topic: gapTopic(d.gapId),
      existingPersonaNames: existingPersonaNames(d.sessionId),
      recent: recentUtterances(d.sessionId, 12),
    });
    const res = await deps.llm.invoke({ prompt: user, system, model: deps.options?.model });
    if (!res.ok) {
      deps.logger.warn({ gap_id: d.gapId, err: res.error }, "facilitator expand llm failed");
      return;
    }
    let gen: GeneratedPersona;
    try {
      gen = extractJsonObject(res.text) as GeneratedPersona;
    } catch (err) {
      deps.logger.warn({ gap_id: d.gapId, err: (err as Error).message }, "facilitator expand parse failed");
      return;
    }
    if (!gen.opening || typeof gen.opening !== "string") return;
    const personaId = `dyn-${randomUUID().slice(0, 8)}`;
    deps.personas.insertOrIgnore({
      id: personaId,
      name: String(gen.name ?? "新視点"),
      display_name: String(gen.display_name ?? "名無し"),
      description: String(gen.description ?? ""),
      traits: Array.isArray(gen.traits) ? gen.traits.map(String) : [],
      speech_style: String(gen.speech_style ?? ""),
    });
    deps.contextProvider.postUtterance({
      workspaceId: deps.workspaceId,
      sessionId: d.sessionId,
      text: gen.opening,
      byPersonaId: personaId,
    });
    deps.logger.info({ gap_id: d.gapId, persona_id: personaId, name: gen.name }, "facilitator expanded discussion");
  }

  async function converge(d: Discussion): Promise<void> {
    const { system, user } = buildConvergePrompt({
      topic: gapTopic(d.gapId),
      recent: recentUtterances(d.sessionId, 40),
    });
    const res = await deps.llm.invoke({ prompt: user, system, model: deps.options?.model });
    if (!res.ok) {
      deps.logger.warn({ gap_id: d.gapId, err: res.error }, "facilitator converge llm failed");
      return;
    }
    let summary: string;
    try {
      const obj = extractJsonObject(res.text) as { summary?: unknown };
      summary = typeof obj.summary === "string" ? obj.summary : res.text;
    } catch {
      summary = res.text;
    }
    deps.contextProvider.postUtterance({
      workspaceId: deps.workspaceId,
      sessionId: d.sessionId,
      text: `【収束】\n${summary}`,
      byPersonaId: FACILITATOR_PERSONA_ID,
    });
    closeGap(d.gapId);
    states.delete(d.sessionId); // 収束したら管理終了 (= ファシリテーター消滅)
    deps.logger.info({ gap_id: d.gapId }, "facilitator converged discussion (gap closed)");
  }

  function closeGap(gapId: string): void {
    const g = raw
      .prepare("SELECT title, description FROM design_gaps WHERE id = ?")
      .get(gapId) as { title: string; description: string | null } | undefined;
    if (!g) return;
    deps.core.repos.designGap.update(gapId, {
      workspaceId: deps.workspaceId,
      title: g.title,
      description: g.description ?? undefined,
      status: "closed",
    });
  }

  async function tickOnce(): Promise<void> {
    const now = Date.now();
    for (const d of listActiveDiscussions()) {
      const metrics = metricsFor(d.sessionId, now);
      const prev = states.get(d.sessionId) ?? { lastUtteranceCount: metrics.utteranceCount, armed: true };
      const { mode, state } = evaluate(metrics, prev, { idleGapMs, maxPersonas });
      states.set(d.sessionId, state);
      try {
        if (mode === "expand") await expand(d);
        else if (mode === "converge") await converge(d);
      } catch (err) {
        deps.logger.warn({ gap_id: d.gapId, err: (err as Error).message }, "facilitator action failed");
      }
    }
  }

  return {
    start(): void {
      if (timer) return;
      timer = setInterval(() => {
        if (running) return;
        running = true;
        tickOnce()
          .catch((err) => deps.logger.warn({ err: (err as Error).message }, "facilitator tick failed"))
          .finally(() => {
            running = false;
          });
      }, tickMs);
    },
    stop(): void {
      if (timer) clearInterval(timer);
      timer = null;
    },
    tickOnce,
  };
}
