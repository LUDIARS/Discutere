/**
 * 未来予測アドバイザリーの進行状態 (セッションごと、プロセス内メモリ)。
 *
 * 本議論の結論 (flow_conclusion) は未来予測より前に書かれるので、結論の有無だけで「完了」と
 * 判定すると、未来予測が走っている間も完了に見える (2026-10-18 Fable / Astra 評価)。
 * Web の status はこの状態を見て、本議論の完了と未来予測の完了を分けて返す。
 *
 * 永続しない理由: 実行中の未来予測はプロセスと一緒に消えるので、再起動後に「実行中」が残ると
 * 永遠に完了しなくなる。再起動後は状態なし (= 走っていない) として扱うのが正しい。
 */

export type ForesightState =
  /** 本議論の実行中 (タグがあり、終わったら未来予測を行う予定)。 */
  | "pending"
  /** 未来予測を実行中。 */
  | "running"
  /** 提案・議論・予測がすべて出た。 */
  | "done"
  /** 行わなかった (本議論が未収束 / 話者不足)。 */
  | "skipped"
  /** 一部の発言か予測が失敗した (出たものは記録済み)。 */
  | "degraded"
  /** 提案が出ず、以降を行わなかった / フェーズが例外で止まった。 */
  | "failed";

export interface ForesightStatus {
  state: ForesightState;
  /** 省略・失敗の理由 (人間向け)。 */
  detail?: string;
}

const statuses = new Map<string, ForesightStatus>();

export function setForesightStatus(sessionId: string, state: ForesightState, detail?: string): void {
  statuses.set(sessionId, detail ? { state, detail } : { state });
}

export function getForesightStatus(sessionId: string): ForesightStatus | null {
  return statuses.get(sessionId) ?? null;
}

/** まだ終わっていない (待機中 / 実行中)。 */
export function isForesightActive(sessionId: string): boolean {
  const s = statuses.get(sessionId)?.state;
  return s === "pending" || s === "running";
}

/** テスト用。 */
export function _resetForesightStatus(): void {
  statuses.clear();
}
