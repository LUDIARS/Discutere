/**
 * Discatier ↔ persona-engine 接続層 (glue)。
 *
 * persona-engine は Discatier の具体型を知らない。 ここで
 * DiscussionContextProvider interface を実装し、 Discatier Core repo に
 * 読み書きする橋渡しを行う。
 *
 * 切り出し時はこの adapter は Discutere 側に残し、 persona-engine package
 * とは分離される。
 */

import type { createCore } from "../core/index.js";
import type {
  ContextGap,
  ContextHypothesis,
  ContextUtterance,
  DiscussionContextProvider,
  PostUtteranceInput,
  ProposeHypothesisInput,
} from "../persona-engine/context-provider.js";

type Core = ReturnType<typeof createCore>;

export interface DiscatierAdapterOptions {
  /** session を 1 つ pin して utterance 投稿先にする (Phase 0 簡易) */
  defaultSessionId?: string;
}

export function createDiscatierContextProvider(
  core: Core,
  options: DiscatierAdapterOptions = {}
): DiscussionContextProvider {
  return {
    listActiveHypotheses(workspaceId, limit): ContextHypothesis[] {
      const rows = core.client.raw
        .prepare(
          `SELECT id, statement, status, design_gap_id, updated_at
             FROM hypotheses
             WHERE workspace_id = ? AND (status IS NULL OR status NOT IN ('rejected'))
             ORDER BY updated_at DESC LIMIT ?`
        )
        .all(workspaceId, limit) as Array<{
        id: string;
        statement: string;
        status: string | null;
        design_gap_id: string | null;
        updated_at: number;
      }>;
      return rows.map((r) => ({
        id: r.id,
        statement: r.statement,
        status: r.status ?? "proposed",
        designGapId: r.design_gap_id,
        updatedAt: r.updated_at,
      }));
    },

    listRecentGaps(workspaceId, limit): ContextGap[] {
      const rows = core.client.raw
        .prepare(
          `SELECT id, title, status, gap_in, expected_affect, observed_affect, updated_at
             FROM design_gaps
             WHERE workspace_id = ?
             ORDER BY updated_at DESC LIMIT ?`
        )
        .all(workspaceId, limit) as Array<{
        id: string;
        title: string;
        status: string | null;
        gap_in: string | null;
        expected_affect: string | null;
        observed_affect: string | null;
        updated_at: number;
      }>;
      return rows.map((r) => {
        const hypIds = core.client.raw
          .prepare(
            "SELECT id FROM hypotheses WHERE workspace_id = ? AND design_gap_id = ?"
          )
          .all(workspaceId, r.id) as Array<{ id: string }>;
        return {
          id: r.id,
          title: r.title,
          status: r.status ?? "open",
          gapIn: r.gap_in,
          expectedAffect: r.expected_affect,
          observedAffect: r.observed_affect,
          hypothesisIds: hypIds.map((h) => h.id),
          updatedAt: r.updated_at,
        };
      });
    },

    listRecentUtterances(workspaceId, sessionId, limit): ContextUtterance[] {
      const rows = core.client.raw
        .prepare(
          `SELECT id, session_id, raw_content, posted_at
             FROM utterances
             WHERE workspace_id = ? AND session_id = ?
             ORDER BY posted_at DESC LIMIT ?`
        )
        .all(workspaceId, sessionId, limit) as Array<{
        id: string;
        session_id: string;
        raw_content: string;
        posted_at: number;
      }>;
      return rows.reverse().map((r) => ({
        id: r.id,
        sessionId: r.session_id,
        rawContent: r.raw_content,
        postedAt: r.posted_at,
      }));
    },

    proposeHypothesis(input: ProposeHypothesisInput): { id: string } {
      const id = core.repos.hypothesis.create({
        workspaceId: input.workspaceId,
        designGapId: input.designGapId ?? undefined,
        statement: input.statement,
        status: "proposed",
      });
      // Phase 0 では「persona が提案」 のメタ情報は learned_notes 側に残す予定。
      // ひとまず raw insert で hypothesis row を即時参照可能にする。
      core.client.raw
        .prepare(
          `INSERT OR IGNORE INTO hypotheses
            (id, workspace_id, design_gap_id, statement, status, integrated, created_at, updated_at)
           VALUES (?, ?, ?, ?, 'proposed', 0, ?, ?)`
        )
        .run(
          id,
          input.workspaceId,
          input.designGapId ?? null,
          input.statement,
          Date.now(),
          Date.now()
        );
      return { id };
    },

    postUtterance(input: PostUtteranceInput): { id: string } {
      const sessionId = input.sessionId || options.defaultSessionId;
      if (!sessionId) {
        throw new Error("postUtterance requires sessionId or adapter.defaultSessionId");
      }
      const id = core.repos.utterance.create({
        workspaceId: input.workspaceId,
        sessionId,
        speakerId: `persona:${input.byPersonaId}`,
        rawContent: input.text,
        postedAt: Date.now(),
        respondsTo: input.respondsTo,
      });
      return { id };
    },
  };
}
