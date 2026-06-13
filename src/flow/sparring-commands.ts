/**
 * 壁打ちフローの終了コマンド検出 (sparring.md)。
 *
 * | ユーザ発話 | 動作 |
 * |---|---|
 * | 「まとめて」 | そのラウンドを終了 (ファシリテーターがまとめ → 続行可) |
 * | 「おわり」   | 議論を終了しまとめに入る (= フロー終了) |
 *
 * 判定方式の決定 (未決事項の確定):
 *   分類器 (LLM) ではなく **文字列一致** を採用する。
 *   理由: コマンドは少数の定型句で、LLM 呼び出しコスト/レイテンシ/誤分類リスクを避けたい
 *   (decision-metrics: 作業コスト小・解決度十分・主目的一致)。短い命令文に限定して誤検知を抑える。
 *   ゆらぎ許容: 末尾の助詞/敬語 (「まとめてください」「終わりにして」) を拾う。
 */

export type SparringCommand = "summarize" | "end" | "none";

/** 句読点・空白・装飾を除いた正規化 (コマンド判定用)。 */
function normalize(text: string): string {
  return text
    .trim()
    .replace(/[。、．，.!?！？\s「」『』]/g, "");
}

const END_TOKENS = ["おわり", "終わり", "おわる", "終わる", "終了", "終わりにして", "おしまい"];
const SUMMARIZE_TOKENS = ["まとめて", "まとめ"];

/**
 * ユーザ発話から壁打ちコマンドを検出する。
 *
 * 誤検知を抑えるため「短い命令文 (正規化後 ~16 文字以内)」のみコマンド扱いとし、
 * 長い意見文に混ざった語は通常発話 (none) として扱う。
 */
export function detectSparringCommand(text: string): SparringCommand {
  const n = normalize(text);
  if (!n) return "none";

  const isShort = n.length <= 16;

  // 終了が優先 (「まとめておわり」のような場合も終了)
  if (END_TOKENS.some((t) => n.includes(t))) {
    // 「終わらない」「終われない」等の否定は除外
    if (/終わ(らな|れな|りた[くか])|おわ(らな|れな)/.test(n)) return "none";
    if (isShort || END_TOKENS.some((t) => n.startsWith(t) || n.endsWith(t))) return "end";
  }

  if (SUMMARIZE_TOKENS.some((t) => n.includes(t))) {
    if (isShort || n.startsWith("まとめ")) return "summarize";
  }

  return "none";
}
