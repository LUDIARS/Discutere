/**
 * 合意スコアラー (FEATURE ③+④) — 各意見発話 (AI persona も 人間も同一に) を AI が評価し、
 * 議論の総意が同意する意見には 👍 リアクション + 合意スコアを記録する。
 *
 * - open な議論 (discussion-of-gap) ごとに、 未採点の意見発話 (persona / 人間、 ext: と
 *   facilitator persona は除外) を最大 8 件集めて 1 回の LLM 呼び出しで合意判定する。
 * - 同意された意見は consensus_scores.score に積み、 既存 opinion_scores にも加点して
 *   facilitator の topOpinions / getOpinionScore に反映する (= まとめが合意意見に引っ張られる)。
 * - discord_message_map に対応 message があり deps.react があれば 👍 を付ける。
 *
 * 人間と AI の発話は完全に同一レイヤで評価する (区別しない)。
 */

import type { createCore } from "../../core/index.js";
import type { LLMClient } from "../llm/client.js";
import type { Logger } from "../types.js";
import { FACILITATOR_PERSONA_ID } from "../facilitator/facilitator.js";

type Core = ReturnType<typeof createCore>;

export const CONSENSUS_EMOJI = "👍";

export interface ConsensusReactInput {
  utteranceId: string;
  channelId: string;
  messageId: string;
  emoji: string;
}

export interface ConsensusScorerDeps {
  core: Core;
  llm: LLMClient;
  intervalMs: number;
  /** discord メッセージへ 👍 を付ける (任意)。 mapping があり set されている時だけ呼ぶ。 */
  react?: (input: ConsensusReactInput) => Promise<void>;
  logger: Logger;
  /** ワークスペース (省略時は scorer 内で gap から自然に絞られる)。 */
  workspaceId?: string;
  /** LLM model 上書き (任意)。 */
  model?: string;
}

export interface ConsensusScorer {
  start(): void;
  stop(): void;
  scoreOnce(): Promise<void>;
}

interface Candidate {
  utteranceId: string;
  speakerId: string;
  content: string;
  human: boolean;
}

interface Discussion {
  sessionId: string;
  gapId: string;
}

const MAX_CANDIDATES = 8;

export function createConsensusScorer(deps: ConsensusScorerDeps): ConsensusScorer {
  const raw = deps.core.client.raw;
  let timer: NodeJS.Timeout | null = null;
  let running = false;

  raw.exec(
    `CREATE TABLE IF NOT EXISTS consensus_scores (
       utterance_id TEXT PRIMARY KEY,
       agree INTEGER NOT NULL,
       score INTEGER NOT NULL,
       reasoning TEXT,
       created_at INTEGER NOT NULL
     )`
  );

  /** open な議論 (facilitator と同じ条件) を列挙。 */
  function listActiveDiscussions(): Discussion[] {
    const rows = raw
      .prepare(
        `SELECT s.id AS sessionId, s.title AS title, s.scene AS scene
           FROM sessions s
          WHERE s.title LIKE 'discussion-of-gap:%'
            AND (s.scene LIKE 'discord:%' OR s.scene LIKE 'gap:%')`
      )
      .all() as Array<{ sessionId: string; title: string }>;
    const out: Discussion[] = [];
    for (const r of rows) {
      const gapId = r.title.slice("discussion-of-gap:".length);
      const gap = raw
        .prepare("SELECT status FROM design_gaps WHERE id = ?")
        .get(gapId) as { status: string | null } | undefined;
      if (!gap) continue;
      if (gap.status && ["closed", "converged", "dismissed"].includes(gap.status)) continue;
      out.push({ sessionId: r.sessionId, gapId });
    }
    return out;
  }

  function isHumanSpeaker(speakerId: string | null): boolean {
    return !!speakerId && !speakerId.startsWith("persona:") && !speakerId.startsWith("ext:");
  }

  /** 未採点の意見発話 (persona / 人間。 ext: と facilitator は除外) を最大 8 件。 */
  function candidatesFor(sessionId: string): Candidate[] {
    const rows = raw
      .prepare(
        `SELECT u.id AS id, u.speaker_id AS speakerId, u.raw_content AS content
           FROM utterances u
          WHERE u.session_id = ?
            AND u.speaker_id IS NOT NULL
            AND u.speaker_id NOT LIKE 'ext:%'
            AND u.speaker_id <> ?
            AND u.id NOT IN (SELECT utterance_id FROM consensus_scores)
            AND u.id NOT IN (SELECT utterance_id FROM utterance_exclusions)
          ORDER BY u.posted_at ASC
          LIMIT ?`
      )
      .all(sessionId, `persona:${FACILITATOR_PERSONA_ID}`, MAX_CANDIDATES) as Array<{
      id: string;
      speakerId: string;
      content: string;
    }>;
    // 【収束】まとめ等のシステム発話は意見ではないので弾く。
    return rows
      .filter((r) => !r.content.startsWith("【収束】"))
      .map((r) => ({
        utteranceId: r.id,
        speakerId: r.speakerId,
        content: r.content,
        human: isHumanSpeaker(r.speakerId),
      }));
  }

  function gapTopic(gapId: string): string {
    const g = raw
      .prepare("SELECT title, description FROM design_gaps WHERE id = ?")
      .get(gapId) as { title: string; description: string | null } | undefined;
    return g ? `${g.title}\n${g.description ?? ""}`.trim() : "(不明な議題)";
  }

  /** discord_message_map から utterance に対応する channel/message を引く (無ければ null)。 */
  function lookupMessage(utteranceId: string): { channelId: string; messageId: string } | null {
    const row = raw
      .prepare(
        "SELECT message_id, channel_id FROM discord_message_map WHERE target_id = ? AND target_kind = 'utterance' ORDER BY created_at DESC LIMIT 1"
      )
      .get(utteranceId) as { message_id: string; channel_id: string | null } | undefined;
    if (!row?.message_id || !row.channel_id) return null;
    return { channelId: row.channel_id, messageId: row.message_id };
  }

  function recordScore(c: Candidate, agree: boolean, score: number, reasoning: string): void {
    raw
      .prepare(
        `INSERT OR REPLACE INTO consensus_scores (utterance_id, agree, score, reasoning, created_at)
         VALUES (?, ?, ?, ?, ?)`
      )
      .run(c.utteranceId, agree ? 1 : 0, score, reasoning, Date.now());
    // 同意された意見は既存 opinion_scores にも加点 (facilitator の topOpinions が拾う)。
    if (agree && score > 0) {
      raw
        .prepare(
          `INSERT INTO opinion_scores (target_id, target_kind, score, updated_at)
             VALUES (?, 'utterance', ?, ?)
           ON CONFLICT(target_id) DO UPDATE SET score = score + excluded.score, updated_at = excluded.updated_at`
        )
        .run(c.utteranceId, score, Date.now());
    }
  }

  async function scoreDiscussion(d: Discussion): Promise<void> {
    const candidates = candidatesFor(d.sessionId);
    if (candidates.length === 0) return;

    const numbered = candidates
      .map((c, i) => `${i}: ${c.content.replace(/\s+/g, " ").slice(0, 300)}`)
      .join("\n");
    const res = await deps.llm.invoke({
      model: deps.model,
      maxTokens: 700,
      system:
        "あなたは議論の合意形成を評価する中立の審査者です。 各意見について、 この議論の総意 (議題の趣旨と他の意見の流れ) が " +
        "その意見に同意するかを判定し、 0〜5 の同意スコアを付けます。 発言者が人間か AI かは一切区別しません。 JSON 配列のみを返してください。",
      prompt: [
        `議題: ${gapTopic(d.gapId)}`,
        "",
        "意見一覧 (番号付き):",
        numbered,
        "",
        '各意見について {"i": 番号, "agree": true/false, "score": 0〜5} を返す。',
        "agree=議論の総意がその意見に賛同するか。 score=同意の強さ (0=反対/無関係, 5=強い合意)。",
        'JSON 配列のみ: [{"i":0,"agree":true,"score":4}, ...]',
      ].join("\n"),
    });
    if (!res.ok) {
      deps.logger.warn({ gap_id: d.gapId, err: res.error }, "consensus scorer llm failed");
      return;
    }

    const verdicts = parseVerdicts(res.text);
    if (!verdicts) {
      deps.logger.warn({ gap_id: d.gapId }, "consensus scorer parse failed");
      return;
    }

    for (const v of verdicts) {
      const c = candidates[v.i];
      if (!c) continue;
      const score = Math.max(0, Math.min(5, Math.round(v.score)));
      recordScore(c, v.agree, score, v.reasoning ?? "");
      // 同意なら discord に 👍 (mapping + react がある時のみ)。
      if (v.agree && score > 0 && deps.react) {
        const msg = lookupMessage(c.utteranceId);
        if (msg) {
          try {
            await deps.react({
              utteranceId: c.utteranceId,
              channelId: msg.channelId,
              messageId: msg.messageId,
              emoji: CONSENSUS_EMOJI,
            });
          } catch (err) {
            deps.logger.warn(
              { utterance_id: c.utteranceId, err: (err as Error).message },
              "consensus scorer react failed"
            );
          }
        }
      }
    }
    deps.logger.info(
      { gap_id: d.gapId, scored: candidates.length },
      "consensus scorer scored discussion"
    );
  }

  async function scoreOnce(): Promise<void> {
    for (const d of listActiveDiscussions()) {
      try {
        await scoreDiscussion(d);
      } catch (err) {
        deps.logger.warn(
          { gap_id: d.gapId, err: (err as Error).message },
          "consensus scorer discussion failed"
        );
      }
    }
  }

  return {
    start(): void {
      if (timer) return;
      timer = setInterval(() => {
        if (running) return;
        running = true;
        scoreOnce()
          .catch((err) => deps.logger.warn({ err: (err as Error).message }, "consensus scorer tick failed"))
          .finally(() => {
            running = false;
          });
      }, deps.intervalMs);
    },
    stop(): void {
      if (timer) clearInterval(timer);
      timer = null;
    },
    scoreOnce,
  };
}

interface Verdict {
  i: number;
  agree: boolean;
  score: number;
  reasoning?: string;
}

function parseVerdicts(text: string): Verdict[] | null {
  const match = text.match(/\[[\s\S]*\]/);
  if (!match) return null;
  try {
    const parsed = JSON.parse(match[0]);
    if (!Array.isArray(parsed)) return null;
    const out: Verdict[] = [];
    for (const item of parsed) {
      if (!item || typeof item !== "object") continue;
      const i = Number((item as Record<string, unknown>).i);
      if (!Number.isInteger(i)) continue;
      out.push({
        i,
        agree: (item as Record<string, unknown>).agree === true,
        score: Number((item as Record<string, unknown>).score) || 0,
        reasoning:
          typeof (item as Record<string, unknown>).reasoning === "string"
            ? ((item as Record<string, unknown>).reasoning as string)
            : undefined,
      });
    }
    return out;
  } catch {
    return null;
  }
}
