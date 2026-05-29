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
}
