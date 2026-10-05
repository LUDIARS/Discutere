/**
 * 途中で止まった議論 (Discutere の再起動などで進行が失われたもの) の「やり直し」判定 (純関数)。
 *
 * 議論の進行状態 (キャスト・ラウンド位置) はプロセスのメモリにしか無いため、再起動で失われると
 * 続きからは再開できない。確定済みのペーパーは discussion_paper に残っているので、
 * スレッドで「再開」等と返信されたら、そのペーパーで議論を最初から回し直す。
 */

/** やり直しの合図とみなす返信 (先頭一致)。 */
const REDO_RE = /^(再開|やり直し|やりなおし|再議論|もう一度|もういちど)/;

export function isRedoText(text: string): boolean {
  return REDO_RE.test(text.trim());
}

/** やり直せる議論タイプ (完走型でペーパーを使うもの)。 */
const REDOABLE_FLOWS = new Set(["discussion", "improvement"]);

export type RedoDecision =
  | { ok: true }
  | { ok: false; reason: "no-paper" | "not-started" | "concluded" | "running" | "unsupported-flow" };

/**
 * やり直してよいかを判定する。
 * - ペーパーが開始済み (status='started') で、結論が無く、このプロセスで進行中でもないこと。
 * - 下書き (status='draft') はレビュー再開の経路が別にあるので対象外。
 */
export function decideRedo(input: {
  paper: { status: string; flow: string } | null;
  concluded: boolean;
  running: boolean;
}): RedoDecision {
  if (!input.paper) return { ok: false, reason: "no-paper" };
  if (input.paper.status !== "started") return { ok: false, reason: "not-started" };
  if (!REDOABLE_FLOWS.has(input.paper.flow)) return { ok: false, reason: "unsupported-flow" };
  if (input.concluded) return { ok: false, reason: "concluded" };
  if (input.running) return { ok: false, reason: "running" };
  return { ok: true };
}
