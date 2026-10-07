/**
 * ペーパーレビュー中のフォーラム返信を、キーワードで 3 種類の指示に振り分ける (純関数・副作用なし)。
 *
 *   - adjust        : 議論内容の調整 (既定。自然文のまま LLM でペーパーへ反映)
 *   - voices        : 外部の声の取り込み (クロール → 集めた声を根拠として反映)
 *   - similar_games : 類似するゲーム (メカニクスの代替として、挙げたゲームのメカニクスを反映)
 *
 * 承認 (「開始」) / 戻す の判定は呼び出し側で先に済ませる前提。
 */

export type PaperReviewIntentKind = "adjust" | "voices" | "similar_games";

export interface PaperReviewIntent {
  kind: PaperReviewIntentKind;
  /** 元の返信本文 (trim 済み)。 */
  text: string;
  /**
   * 指示から取り出した対象語。voices = 検索語 (無ければ議題で検索)、similar_games = ゲーム名。
   * adjust では空。
   */
  terms: string[];
}

/** 種類ごとの表示名 (スレッド通知に使う)。 */
export const PAPER_REVIEW_INTENT_LABELS: Record<PaperReviewIntentKind, string> = {
  adjust: "議論内容の調整",
  voices: "外部の声の取り込み",
  similar_games: "類似するゲーム (メカニクスの代替)",
};

const VOICES_RE =
  /(外部の声|ユーザー?の声|プレイヤーの声|生の声|口コミ|(感想|レビュー|コメント|反応)を?(取り込|取りこ|集め|拾|追加|入れ))/;
const SIMILAR_GAMES_RE =
  /(類似(する|の)?(ゲーム|作品|タイトル)|似た(ような)?(ゲーム|作品|タイトル)|似ている(ゲーム|作品|タイトル)|参考(になる|の)?(ゲーム|作品)|メカニクスの代替|代替(の)?(ゲーム|メカニクス))/;

const QUOTED_RE = /[「『"“]([^」』"”]+)[」』"”]/g;

/** 対象語の取り出しで捨てる語 (指示の言い回し)。 */
const FILLER_RE =
  /(類似(する|の)?|似た(ような)?|似ている|参考(になる|の)?|代替(の)?|メカニクス|ゲーム|作品|タイトル|外部の声|ユーザー?の声|プレイヤーの声|生の声|口コミ|感想|レビュー|コメント|反応|として|を|に|で|から|の|は|も|追加|取り込(んで|む|み)?|取りこ(んで|む)?|集め(て|る)?|拾(って|う)?|入れ(て|る)?|参照|使(って|う)|して|ください|下さい|お願い(します)?|反映)/g;

function quotedTerms(text: string): string[] {
  return [...text.matchAll(QUOTED_RE)].map((m) => m[1]!.trim()).filter(Boolean);
}

/** 引用が無い時の素朴な抽出: 「:」以降 (無ければ全体) から言い回しを落として区切る。 */
function looseTerms(text: string): string[] {
  const colon = text.search(/[:：]/);
  const body = colon >= 0 ? text.slice(colon + 1) : text;
  return body
    .replace(FILLER_RE, " ")
    .split(/[、,，・/／\s]+|と(?=\S)/)
    .map((t) => t.replace(/[。.!！?？]+$/u, "").trim())
    .filter((t) => t.length >= 2);
}

function uniq(values: string[]): string[] {
  return [...new Set(values)];
}

/**
 * 調整指示になっていない短い返信 (相づち・否定・別の質問への回答)。ペーパーを書き換えない。
 * 例: 同じスレッドの別ボット (Concordia) の質問に「ない」と答えた返信が調整指示として
 * ペーパーへ反映され、ペーパーが勝手に書き換わっていた (2026-10-07)。
 * 「ok」「了解です」などの承認語は呼び出し側で先に isApprovalText で拾う前提。
 */
const NON_INSTRUCTION_REPLIES = new Set([
  "ない", "無い", "なし", "無し", "ないです", "なしです", "ありません", "特にない", "特になし",
  "とくにない", "とくになし", "なにもない", "何もない", "大丈夫", "だいじょうぶ", "大丈夫です",
  "はい", "いいえ", "うん", "ううん", "いいよ", "りょうかい", "了解", "わかった", "分かった",
  "ありがとう", "ありがとうございます", "どうも", "そう", "そうです", "違う", "ちがう",
  "no", "none", "nope", "yes", "yep", "thanks", "thx",
]);

/** 指示になっていない返信か (相づち・否定・1 文字以下・記号や絵文字だけ)。 */
export function isNonInstructionReply(raw: string): boolean {
  const t = raw.trim().toLowerCase().replace(/[。、.,!！?？~〜ー\s]+$/u, "").replace(/^[\s「『]+|[」』]+$/gu, "");
  if (t.length <= 1) return true;
  if (!/[\p{L}\p{N}]/u.test(t)) return true; // 記号・絵文字だけ
  return NON_INSTRUCTION_REPLIES.has(t);
}

/** 返信本文を 3 種類の指示に振り分ける。両方のキーワードがあれば外部の声を優先する。 */
export function classifyPaperReviewIntent(raw: string): PaperReviewIntent {
  const text = raw.trim();
  if (VOICES_RE.test(text)) {
    return { kind: "voices", text, terms: uniq(quotedTerms(text)) };
  }
  if (SIMILAR_GAMES_RE.test(text)) {
    const quoted = quotedTerms(text);
    return { kind: "similar_games", text, terms: uniq(quoted.length > 0 ? quoted : looseTerms(text)) };
  }
  return { kind: "adjust", text, terms: [] };
}
