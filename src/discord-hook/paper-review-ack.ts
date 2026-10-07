/**
 * ペーパーレビュー中の回答を受け取ったことをリアクションで示す。
 * 「ない」「はい」など編集を伴わない回答も対象。Discord のリプライは対象外。
 *
 * ✅ は「ペーパー承認 (議論開始)」の操作に使っているので、確認済みの印には ☑️ を使う
 * (bot のリアクションは MessageReactionAdd 側で無視されるため承認とは混ざらない)。
 */

/** 確認済みの印。 */
export const PAPER_REVIEW_ACK_EMOJI = "☑️";

/** リアクションを付けられる返信 (discord.js Message の必要部分)。 */
export interface AckableMessage {
  content: string;
  reference?: unknown;
  react(emoji: string): Promise<unknown>;
}

/**
 * レビュー中スレッドへの本文ありの返信に確認済みリアクションを付ける。
 * 付与失敗 (権限不足など) は warn だけで、返信処理は止めない。
 * @returns リアクションを試みたら true。
 */
export async function ackPaperReviewReply(
  msg: AckableMessage,
  inReview: boolean,
  warn: (m: string) => void = console.warn
): Promise<boolean> {
  if (!inReview || msg.reference || !msg.content.trim()) return false;
  try {
    await msg.react(PAPER_REVIEW_ACK_EMOJI);
  } catch (err) {
    warn(`  discord-forum: ペーパー返信への確認リアクション失敗: ${(err as Error).message}`);
  }
  return true;
}
