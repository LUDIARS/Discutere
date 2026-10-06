/**
 * Discord ペーパーレビューの単一セッション管理 (スレッド単位)。
 *
 * ペーパー確定 (承認) までは 1 スレッド = 1 セッションで扱う:
 *   - preparing: 情報収集〜草案作成中。同じスレッドで準備を二重に始めない (再投稿の防止)。
 *     この間に届いた返信は捨てずに溜め、草案を出した後で順に処理する。
 *   - reviewing: 草案を出して調整/承認を待っている。
 * 返信・承認・自動開始はスレッドごとに直列化し、同時処理でペーパーが重複投稿されないようにする。
 * フロー再提案 (議論タイプ選択メニュー) は 1 セッションにつき 1 回だけ出す。
 *
 * メモリ上の状態だけを持つ (DB・Discord に依存しない)。草案そのものは discord-live が持つ。
 */

export type ReviewSessionState = "preparing" | "reviewing";

interface ReviewSession {
  state: ReviewSessionState;
  /** preparing 中に届いた返信 (届いた順)。 */
  buffered: string[];
  /** フロー再提案を出したか。 */
  reproposed: boolean;
}

const sessions = new Map<string, ReviewSession>();
/** スレッドごとの直列化キュー (末尾の Promise)。セッションの有無と無関係に使う。 */
const tails = new Map<string, Promise<unknown>>();

/** テスト用: 全セッションとキューを破棄する。 */
export function _resetReviewSessions(): void {
  sessions.clear();
  tails.clear();
}

/**
 * 準備中セッションを開く。すでにセッションがあれば開かずに false (= 準備を二重に始めない)。
 */
export function openPreparing(threadId: string): boolean {
  if (sessions.has(threadId)) return false;
  sessions.set(threadId, { state: "preparing", buffered: [], reproposed: false });
  return true;
}

/** 草案を出す前の準備中か。 */
export function isPreparing(threadId: string): boolean {
  return sessions.get(threadId)?.state === "preparing";
}

/** 準備中またはレビュー中のセッションがあるか。 */
export function hasReviewSession(threadId: string): boolean {
  return sessions.has(threadId);
}

/** 準備中に届いた返信を溜める。準備中でなければ溜めずに false。 */
export function bufferReply(threadId: string, text: string): boolean {
  const s = sessions.get(threadId);
  if (!s || s.state !== "preparing") return false;
  s.buffered.push(text);
  return true;
}

/**
 * レビュー中へ移し、準備中に溜まった返信を取り出す (取り出した分は消える)。
 * セッションが無ければ (再起動後の再開など) レビュー中として開く。
 */
export function markReviewing(threadId: string): string[] {
  const s = sessions.get(threadId);
  if (!s) {
    sessions.set(threadId, { state: "reviewing", buffered: [], reproposed: false });
    return [];
  }
  s.state = "reviewing";
  const buffered = s.buffered;
  s.buffered = [];
  return buffered;
}

/** フロー再提案を出してよいか。1 セッションにつき最初の 1 回だけ true。 */
export function claimRepropose(threadId: string): boolean {
  const s = sessions.get(threadId);
  if (!s || s.reproposed) return false;
  s.reproposed = true;
  return true;
}

/** セッションを閉じる (承認・破棄・準備失敗)。溜まった返信も捨てる。 */
export function closeReviewSession(threadId: string): void {
  sessions.delete(threadId);
}

/**
 * スレッドごとに task を直列実行する。前の task が失敗しても後続は走る。
 * @returns task の結果。
 */
export function runSerial<T>(threadId: string, task: () => Promise<T>): Promise<T> {
  const prev = tails.get(threadId) ?? Promise.resolve();
  const next = prev.then(task, task);
  const settled = next.catch(() => undefined);
  tails.set(threadId, settled);
  void settled.then(() => {
    if (tails.get(threadId) === settled) tails.delete(threadId);
  });
  return next;
}
