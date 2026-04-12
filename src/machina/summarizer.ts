/**
 * M3 MACHINA: チャットログ要約エンジン
 *
 * 取り込まれた Slack/Discord メッセージを期間単位で集約し、
 * 要約テキスト・トピック候補・参加者統計を生成する。
 *
 * 現在はルールベースの要約 (参加者集計 + キーワード抽出 + 代表メッセージ)。
 * 将来的に Claude API 等の LLM 要約に差し替え可能。
 */

type ChatMessageLike = {
  id: string;
  authorId: string;
  authorName: string;
  text: string;
  postedAt: Date;
};

export interface SummaryHighlights {
  participants: Array<{ authorId: string; authorName: string; messageCount: number }>;
  topKeywords: Array<{ keyword: string; count: number }>;
  topMessages: Array<{ authorName: string; text: string; postedAt: string }>;
}

export interface SummaryResult {
  summary: string;
  highlights: SummaryHighlights;
  messageCount: number;
}

// 日本語/英語の代表的なストップワード
const STOPWORDS = new Set([
  "the", "and", "for", "you", "that", "this", "with", "are", "was", "but",
  "not", "have", "has", "had", "get", "got", "one", "all", "can", "will",
  "する", "した", "して", "される", "なる", "なった", "ある", "ない", "だ",
  "です", "ます", "こと", "もの", "ため", "よう", "これ", "それ", "あれ",
  "どれ", "私", "我々", "自分", "ちょっと", "本当", "やっぱり", "まじ",
  "まあ", "ちな", "あと", "って", "けど", "でも", "とか", "から", "まで",
]);

/**
 * メッセージ群から要約を生成
 */
export function summarizeMessages(messages: ChatMessageLike[]): SummaryResult {
  if (messages.length === 0) {
    return {
      summary: "対象期間にメッセージはありませんでした。",
      highlights: { participants: [], topKeywords: [], topMessages: [] },
      messageCount: 0,
    };
  }

  // 時系列順に並び替え
  const ordered = [...messages].sort(
    (a, b) => a.postedAt.getTime() - b.postedAt.getTime()
  );

  // 参加者集計
  const participantMap = new Map<string, { authorName: string; count: number }>();
  for (const m of ordered) {
    const existing = participantMap.get(m.authorId);
    if (existing) {
      existing.count += 1;
    } else {
      participantMap.set(m.authorId, { authorName: m.authorName, count: 1 });
    }
  }
  const participants = Array.from(participantMap.entries())
    .map(([authorId, v]) => ({
      authorId,
      authorName: v.authorName,
      messageCount: v.count,
    }))
    .sort((a, b) => b.messageCount - a.messageCount);

  // キーワード抽出 (語彙ベース)
  const keywordMap = new Map<string, number>();
  for (const m of ordered) {
    const tokens = tokenize(m.text);
    for (const token of tokens) {
      if (STOPWORDS.has(token)) continue;
      if (token.length < 2) continue;
      keywordMap.set(token, (keywordMap.get(token) ?? 0) + 1);
    }
  }
  const topKeywords = Array.from(keywordMap.entries())
    .filter(([, count]) => count >= 2)
    .sort((a, b) => b[1] - a[1])
    .slice(0, 10)
    .map(([keyword, count]) => ({ keyword, count }));

  // 代表メッセージ: 長めのメッセージ上位3件
  const topMessages = [...ordered]
    .sort((a, b) => b.text.length - a.text.length)
    .slice(0, 3)
    .map((m) => ({
      authorName: m.authorName,
      text: m.text.slice(0, 200),
      postedAt: m.postedAt.toISOString(),
    }));

  // サマリ本文
  const firstAt = ordered[0].postedAt;
  const lastAt = ordered[ordered.length - 1].postedAt;
  const durationHours = Math.max(
    1,
    Math.round((lastAt.getTime() - firstAt.getTime()) / 3_600_000)
  );

  const parts: string[] = [];
  parts.push(
    `${messages.length} 件のメッセージ (${participants.length} 人の参加者) が約 ${durationHours} 時間にわたって投稿されました。`
  );
  if (participants.length > 0) {
    const top = participants.slice(0, 3).map((p) => `${p.authorName} (${p.messageCount})`);
    parts.push(`主な発言者: ${top.join(", ")}`);
  }
  if (topKeywords.length > 0) {
    parts.push(
      `頻出キーワード: ${topKeywords.slice(0, 5).map((k) => k.keyword).join(", ")}`
    );
  }

  return {
    summary: parts.join("\n"),
    highlights: { participants, topKeywords, topMessages },
    messageCount: messages.length,
  };
}

/**
 * 簡易トークナイザ — 記号除去、カタカナ/ひらがな/英字のみ抽出
 */
function tokenize(text: string): string[] {
  const cleaned = text
    .replace(/<@[\w]+>/g, " ")
    .replace(/https?:\/\/\S+/g, " ")
    .replace(/[、。,.!！?？:：;；()（）"'`]/g, " ")
    .toLowerCase();
  return cleaned
    .split(/\s+/)
    .map((t) => t.trim())
    .filter((t) => t.length > 0);
}
