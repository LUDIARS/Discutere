/**
 * フロー共通タグ体系 (OVERVIEW §3)。
 *
 * 機密度+観点タグ。外部データ収集の可否と
 * ディスカッションペーパーの補足内容を決める。
 * Discord フォーラムタグとは別軸。
 */

export type FlowTag = "機密" | "内部" | "運用" | "開発" | (string & {});

/** 収集禁止タグ群 */
const COLLECTION_DENY = new Set<string>(["機密", "内部"]);

/**
 * 外部データ収集 (YouTube/Steam 等) が許可されるか。
 * `機密` か `内部` タグが 1 つでも含まれると false。
 */
export function canCollectExternal(tags: readonly FlowTag[]): boolean {
  return !tags.some((t) => COLLECTION_DENY.has(t));
}

/**
 * タグに応じたディスカッションペーパーへの観点補足テキストを返す。
 * 複数タグがある場合は全て連結する。
 */
export function paperSupplement(tags: readonly FlowTag[]): string {
  const lines: string[] = [];
  if (tags.includes("開発")) {
    lines.push("【開発者観点】設計意図・実装コスト・技術的トレードオフを踏まえて議論してください。");
  }
  if (tags.includes("運用")) {
    lines.push("【運用者観点】継続率・収益・サーバ負荷・運用コストを踏まえて議論してください。");
  }
  if (tags.includes("内部")) {
    lines.push("【内部情報】外部には非公開の社内情報・未発表の情報を前提に議論してください。");
  }
  if (tags.includes("機密")) {
    lines.push("【機密】実ユーザデータが使用できないため、メカニクス評価から合成した想定意見データを参考にしています。");
  }
  return lines.join("\n");
}
