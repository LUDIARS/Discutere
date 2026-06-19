/**
 * 結論タイトルから「話題のキーワード」を素朴に抽出する (materialCount 算出用)。
 *
 * 形態素解析器は持たない (依存ゼロ方針) ため、次の素朴規則で拾う:
 *   1. 「…」で括られた語 (ゲーム名 / 主題) があれば、その内側を最優先で採る。
 *      括弧内の英名 (例「エルデンリング (Elden Ring)」) は和名・英名の両方を term にする。
 *   2. 「」が無い (トレンド議題等) なら、最初のダッシュ手前を区切り文字で割って
 *      定型語を除いた長さ 3 以上のトークンを拾う。
 * いずれも純関数 — 単体テスト可能。
 */

/** タイトル定型・助詞・汎用語 (キーワードとして無意味なので落とす)。 */
const STOP = new Set([
  "何が面白いのか",
  "について",
  "とは",
  "観点",
  "人気",
  "面白さ",
  "ゲーム体験",
  "複数",
  "視点",
  "構造的",
  "分析する",
  "本当に",
  "どこから",
  "どこ",
  "なぜ",
  "どう",
]);

/** 「…」内側 (ゲーム名/主題) を term にする。和名と括弧内英名の両方。 */
function termsFromQuoted(inner: string, out: Set<string>): void {
  const jp = inner.split(/[（(]/)[0].trim();
  if (jp.length >= 2) out.add(jp);
  const rom = inner.match(/[（(]([^）)]+)[）)]/);
  if (rom && rom[1].trim().length >= 2) out.add(rom[1].trim());
}

export function topicTermsFromTitle(title: string, max = 5): string[] {
  const terms = new Set<string>();
  const quoted = title.match(/「([^」]+)」/g);
  if (quoted && quoted.length > 0) {
    // 最初の「」(= ゲーム名/主題)。後続の「angle」等は議題装飾なので採らない。
    termsFromQuoted(quoted[0].slice(1, -1), terms);
  } else {
    const head = title.split(/[—\-–]/)[0];
    for (const tok of head.split(/[\s、・,/。：:；;（）()「」"'!?！？]+/)) {
      const t = tok.trim();
      if (t.length >= 3 && !STOP.has(t)) terms.add(t);
    }
  }
  return [...terms].slice(0, max);
}
