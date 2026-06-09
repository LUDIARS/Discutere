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
  /** 議題の説明文 (対象ゲーム/ジャンルのアンカー + 投稿の要点)。persona が議論の前提として読む。 */
  description?: string | null;
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

/**
 * 議題に関連する「外部の生の声」(RAG で prompt に注入する素材、 §14)。
 * 個人はペルソナ名へマスク済 (speakerLabel)、 出所 (source/sourceUrl) は開示する (§6)。
 */
export interface ContextExternalVoice {
  id: string;
  content: string;
  /** ソース種別 (youtube / niconico / steam / ...)。 露出可。 */
  source: string;
  /** 元 URL (attribution があれば)。 露出可。 */
  sourceUrl: string | null;
  /** 露出用の仮名ラベル (例 `論者#a1b2`)。 公開 ID は出さない。 */
  speakerLabel: string;
  /** キーワード一致 + opinion-score の合成スコア (高いほど関連/支持)。 */
  score: number;
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

  /**
   * 議題語 (terms) に関連する外部の生の声を active KG から取得する (RAG / §14)。
   * キーワード一致 + 既存 opinion-score の合成スコア順で上位 limit 件。
   * 個人はペルソナ名へマスク、 出所 (source/sourceUrl) を付与する (§6)。
   * 実装は任意 (未実装なら prompt に外部の声は載らない = 従来挙動)。
   */
  listRelevantExternalVoices?(
    workspaceId: string,
    terms: string[],
    limit: number
  ): ContextExternalVoice[];

  /**
   * 指定 gap への「進行役への調整指示」 (参加者が Discord で出したトーン/語り口の指示) を
   * 新しい順 limit 件、 時系列 (古い→新しい) の本文配列で返す。 prompt の議題ブロックに
   * 注入し、 発話が指示に沿うようにする。 実装は任意 (未実装なら従来挙動)。
   */
  listFacilitatorDirectives?(workspaceId: string, gapId: string, limit: number): string[];

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
