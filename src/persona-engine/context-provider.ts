/**
 * DiscussionContextProvider — persona-engine が議論データを読み書きする境界。
 *
 * 切り出し時は consumer (Discatier / Concordia / 他) が interface を実装する。
 * persona-engine 内部は Discutere の Game / Mechanic / Hypothesis 等の具体型を
 * 一切 import しない (= context-provider 経由で構造化データだけ取る)。
 */

export interface ContextHypothesis {
  id: string;
  statement: string;
  status: string;
  designGapId: string | null;
  updatedAt: number;
}

export interface ContextGap {
  id: string;
  title: string;
  status: string;
  gapIn: string | null;
  expectedAffect: string | null;
  observedAffect: string | null;
  hypothesisIds: string[];
  updatedAt: number;
}

export interface ContextUtterance {
  id: string;
  sessionId: string;
  rawContent: string;
  postedAt: number;
}

export interface ProposeHypothesisInput {
  workspaceId: string;
  statement: string;
  designGapId?: string | null;
  byPersonaId: string;
  reasoning?: string;
}

export interface PostUtteranceInput {
  workspaceId: string;
  sessionId: string;
  text: string;
  byPersonaId: string;
  respondsTo?: string;
}

export interface DiscussionContextProvider {
  listActiveHypotheses(workspaceId: string, limit: number): ContextHypothesis[];
  listRecentGaps(workspaceId: string, limit: number): ContextGap[];
  listRecentUtterances(
    workspaceId: string,
    sessionId: string,
    limit: number
  ): ContextUtterance[];

  proposeHypothesis(input: ProposeHypothesisInput): { id: string };
  postUtterance(input: PostUtteranceInput): { id: string };

  /**
   * tick rule 等 session context が無い発話のために、 hypothesis / gap から
   * 紐付く議論 session (discussion-of-gap:<gapId>) を引く。 無ければ null。
   * 実装は任意 (未実装なら tick 発話は skip される)。
   */
  findDiscussionSession?(input: {
    workspaceId: string;
    hypothesisId?: string;
    gapId?: string;
  }): string | null;

  /**
   * 純 debate 発話 (hypothesis_id も sessionId も無い tick rule) のための着地先。
   * 「いま進行中の discord-bound 議論 session」 = 最新の open gap (closed/converged/
   * dismissed でない) の discussion-of-gap session で discord scene かつ未終了のもの。
   * これが無いと debate ペルソナの post_utterance は session を解決できず skip され、
   * Discord に発話が一切流れない。 無ければ null。
   */
  findActiveDiscussionSession?(input: { workspaceId: string }): string | null;
}
