/**
 * 人物パック形成のジャンル辞書 (統合レビュー §2.1「範囲」、S5)。
 *
 * ジャンルは LLM で推定しない。版付きの静的辞書でゲーム (gameSlug) → ジャンル id[] を引く。
 * 辞書に無いゲームは未分類として扱い、どのジャンルにも入れない (別ジャンルの発言で補わない)。
 */

const GENRE_ID = /^[a-z0-9][a-z0-9_-]{0,63}$/;

export interface GenreDictionary {
  specVersion: number;
  /** gameSlug → ジャンル id の配列 (1 ゲームが複数ジャンルに属してよい)。 */
  games: Record<string, string[]>;
  /** gameSlug → 表示用のゲーム名 (無ければ gameSlug を使う)。 */
  titles: Record<string, string>;
}

/** 辞書 JSON を検証して読む。形が違えば throw (形成を黙って空にしない)。 */
export function parseGenreDictionary(raw: unknown): GenreDictionary {
  if (!raw || typeof raw !== "object") throw new Error("genre dictionary: オブジェクトが必要");
  const o = raw as Record<string, unknown>;
  if (typeof o.specVersion !== "number" || !Number.isInteger(o.specVersion) || o.specVersion < 1) {
    throw new Error("genre dictionary: specVersion (1 以上の整数) が必要");
  }
  if (!o.games || typeof o.games !== "object") throw new Error("genre dictionary: games が必要");
  const games: Record<string, string[]> = {};
  for (const [slug, genres] of Object.entries(o.games as Record<string, unknown>)) {
    if (!Array.isArray(genres) || genres.some((g) => typeof g !== "string" || !GENRE_ID.test(g))) {
      throw new Error(`genre dictionary: games.${slug} はジャンル id の配列`);
    }
    games[slug] = [...new Set(genres as string[])];
  }
  const titles: Record<string, string> = {};
  if (o.titles && typeof o.titles === "object") {
    for (const [slug, title] of Object.entries(o.titles as Record<string, unknown>)) {
      if (typeof title === "string" && title.trim()) titles[slug] = title.trim();
    }
  }
  return { specVersion: o.specVersion, games, titles };
}

export function genresOf(dict: GenreDictionary, gameSlug: string | undefined): string[] {
  return gameSlug ? (dict.games[gameSlug] ?? []) : [];
}

export function gameTitleOf(dict: GenreDictionary, gameSlug: string): string {
  return dict.titles[gameSlug] ?? gameSlug;
}
