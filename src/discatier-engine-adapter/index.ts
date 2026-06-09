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
  ContextExternalVoice,
  ContextGap,
  ContextHypothesis,
  ContextUtterance,
  DiscussionContextProvider,
  PostUtteranceInput,
  ProposeHypothesisInput,
} from "../persona-engine/context-provider.js";
import { ensureReactionTables, getOpinionScore } from "../discord-hook/reactions.js";
import { listFacilitatorDirectives } from "../discord-hook/facilitator-directives.js";
import { listExcludedIds } from "../core/noise/exclusions.js";
import { openAttributionStore } from "../crawler/sources/attribution-store.js";
import { maskedPersonaLabel } from "../crawler/sources/persona.js";

type Core = ReturnType<typeof createCore>;

export interface DiscatierAdapterOptions {
  /** session を 1 つ pin して utterance 投稿先にする (Phase 0 簡易) */
  defaultSessionId?: string;
  /**
   * PR-E #5-2: persona 発言の md 自動 export 先 root (= data/discussions の親)。
   * 指定時、 postUtterance / proposeHypothesis 後に該当 md を生成する。
   * 失敗は warn のみで投稿自体は成功扱い (= 議論を止めない)。
   */
  autoExportRootDir?: string;
  /** auto-export 失敗時のログハンドラ (default: console.warn) */
  onAutoExportError?: (err: unknown, ctx: { type: string; id: string }) => void;
  /**
   * PR-I: 議論 utterance / hypothesis を作った直後に発火する callback。
   * Discord 議論 bridge から差し込む (= AI 発話を Discord channel に投稿)。
   * 失敗しても adapter 側は無視 (= 議論を止めない)。
   */
  onPostedUtterance?: (input: {
    workspaceId: string;
    sessionId: string;
    text: string;
    byPersonaId: string;
    utteranceId: string;
  }) => Promise<void> | void;
  onPostedHypothesis?: (input: {
    workspaceId: string;
    statement: string;
    byPersonaId: string;
    designGapId: string | null;
    hypothesisId: string;
  }) => Promise<void> | void;
}

export function createDiscatierContextProvider(
  core: Core,
  options: DiscatierAdapterOptions = {}
): DiscussionContextProvider {
  const autoExport = options.autoExportRootDir;
  const onErr =
    options.onAutoExportError ?? ((err, ctx) => console.warn("[adapter:auto-export]", ctx, err));
  return {
    listActiveHypotheses(workspaceId, limit): ContextHypothesis[] {
      // gap が closed/converged になった議論の hypothesis は context から外す。
      // これを残すと facilitator が収束させても refute-cold / integrate-on-many
      // (tick rule) がそれらを拾い続け、 議論が止まらない (= 収束無効化)。
      const rows = core.client.raw
        .prepare(
          `SELECT h.id, h.statement, h.status, h.design_gap_id, h.updated_at
             FROM hypotheses h
             LEFT JOIN design_gaps g ON g.id = h.design_gap_id
             WHERE h.workspace_id = ?
               AND (h.status IS NULL OR h.status NOT IN ('rejected'))
               AND (g.status IS NULL OR g.status NOT IN ('closed', 'converged'))
             ORDER BY h.updated_at DESC LIMIT ?`
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
          `SELECT id, title, description, status, gap_in, expected_affect, observed_affect, updated_at
             FROM design_gaps
             WHERE workspace_id = ?
             ORDER BY updated_at DESC LIMIT ?`
        )
        .all(workspaceId, limit) as Array<{
        id: string;
        title: string;
        description: string | null;
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
          description: r.description,
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

    listRelevantExternalVoices(workspaceId, terms, limit): ContextExternalVoice[] {
      if (limit <= 0) return [];
      // 候補: 取り込み済み外部発話 (speaker_id が ext:%) を直近から束で取り、 スコアして上位を返す。
      // active KG に閉じる (workspace_id 一致)。 大量取り込みでも候補数を CAP で抑える。
      const CANDIDATE_CAP = 300;
      const CONTENT_CAP = 200; // prompt トークン節約: 1 件の本文上限。
      const rows = core.client.raw
        .prepare(
          `SELECT id, speaker_id, raw_content
             FROM utterances
            WHERE workspace_id = ? AND speaker_id LIKE 'ext:%'
            ORDER BY posted_at DESC LIMIT ?`
        )
        .all(workspaceId, CANDIDATE_CAP) as Array<{
        id: string;
        speaker_id: string | null;
        raw_content: string;
      }>;
      if (rows.length === 0) return [];
      // opinion_scores はリアクション系テーブル。 boot 前/単体呼び出しでも落ちないよう冪等保証。
      ensureReactionTables(core.client.raw);
      const excluded = listExcludedIds(core.client.raw);
      const keyTerms = Array.from(
        new Set(terms.map((t) => t.trim().toLowerCase()).filter((t) => t.length >= 2))
      );
      const attribution = openAttributionStore();
      try {
        const scored = rows
          .filter((r) => !excluded.has(r.id))
          .map((r) => {
            const content = r.raw_content ?? "";
            const lc = content.toLowerCase();
            const hits = keyTerms.reduce((n, t) => (lc.includes(t) ? n + 1 : n), 0);
            const opinion = getOpinionScore(core.client.raw, r.id);
            // キーワード一致を主、 opinion-score (合意/支持) を従にした合成スコア。
            return { r, score: hits * 10 + opinion };
          })
          // 関連語ヒットも支持スコアも無い声は議論を薄めるので落とす。
          .filter((x) => x.score > 0)
          .sort((a, b) => b.score - a.score)
          .slice(0, limit);
        return scored.map(({ r, score }) => {
          const a = attribution.get(r.id);
          const content = r.raw_content ?? "";
          return {
            id: r.id,
            content: content.length > CONTENT_CAP ? content.slice(0, CONTENT_CAP) + "…" : content,
            source: a?.source ?? "external",
            sourceUrl: a?.sourceUrl ?? null,
            // §6: 個人 (speaker_id = 公開 ID) はペルソナ名へマスク。 公開 ID は出さない。
            speakerLabel: r.speaker_id ? maskedPersonaLabel(r.speaker_id) : "外部の声",
            score,
          };
        });
      } finally {
        attribution.close();
      }
    },

    listFacilitatorDirectives(workspaceId, gapId, limit): string[] {
      // サイドカー表は listFacilitatorDirectives 内で冪等確保される (boot 前でも落ちない)。
      return listFacilitatorDirectives(core.client.raw, workspaceId, gapId, limit);
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
      autoExportIfEnabled("hyp", id, input.workspaceId);
      if (options.onPostedHypothesis) {
        Promise.resolve(
          options.onPostedHypothesis({
            workspaceId: input.workspaceId,
            statement: input.statement,
            byPersonaId: input.byPersonaId,
            designGapId: input.designGapId ?? null,
            hypothesisId: id,
          })
        ).catch((err) => onErr(err, { type: "hyp", id }));
      }
      return { id };
    },

    findDiscussionSession(input: {
      workspaceId: string;
      hypothesisId?: string;
      gapId?: string;
    }): string | null {
      let gapId = input.gapId;
      if (!gapId && input.hypothesisId) {
        const row = core.client.raw
          .prepare("SELECT design_gap_id FROM hypotheses WHERE id = ?")
          .get(input.hypothesisId) as { design_gap_id: string | null } | undefined;
        gapId = row?.design_gap_id ?? undefined;
      }
      if (!gapId) return null;
      // closed/converged gap の議論には tick rule を着地させない (= 収束後の沈黙)。
      const gap = core.client.raw
        .prepare("SELECT status FROM design_gaps WHERE id = ?")
        .get(gapId) as { status: string | null } | undefined;
      if (gap?.status && ["closed", "converged", "dismissed"].includes(gap.status)) return null;
      // ended_at が入った session (= 管理画面から「閉じた」議論) には着地させない。
      const session = core.client.raw
        .prepare(
          "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? AND ended_at IS NULL ORDER BY started_at DESC LIMIT 1"
        )
        .get(input.workspaceId, `discussion-of-gap:${gapId}`) as { id: string } | undefined;
      return session?.id ?? null;
    },

    findActiveDiscussionSession(input: { workspaceId: string }): string | null {
      // hypothesis も sessionId も無い純 debate 発話の着地先。 進行中 (未終了・未収束)
      // の discord-bound 議論 session のうち最新のものを 1 件返す。
      const row = core.client.raw
        .prepare(
          `SELECT s.id AS id
             FROM sessions s
             JOIN design_gaps g
               ON g.id = SUBSTR(s.title, LENGTH('discussion-of-gap:') + 1)
            WHERE s.workspace_id = ?
              AND s.title LIKE 'discussion-of-gap:%'
              AND s.scene LIKE 'discord:%'
              AND s.ended_at IS NULL
              AND (g.status IS NULL OR g.status NOT IN ('closed','converged','dismissed','resolved','rejected'))
            ORDER BY s.started_at DESC LIMIT 1`
        )
        .get(input.workspaceId) as { id: string } | undefined;
      return row?.id ?? null;
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
      autoExportIfEnabled("utt", id, input.workspaceId);
      if (options.onPostedUtterance) {
        Promise.resolve(
          options.onPostedUtterance({
            workspaceId: input.workspaceId,
            sessionId,
            text: input.text,
            byPersonaId: input.byPersonaId,
            utteranceId: id,
          })
        ).catch((err) => onErr(err, { type: "utt", id }));
      }
      return { id };
    },
  };

  function autoExportIfEnabled(type: "utt" | "hyp", id: string, workspaceId: string): void {
    if (!autoExport) return;
    // dump は side-effect import で重い。 ここで動的 require して循環依存回避。
    import("../visualize/dump.js")
      .then(({ dumpNode }) => {
        try {
          dumpNode(core, type, id, { workspaceId, rootDir: autoExport });
        } catch (err) {
          onErr(err, { type, id });
        }
      })
      .catch((err) => onErr(err, { type, id }));
  }
}
