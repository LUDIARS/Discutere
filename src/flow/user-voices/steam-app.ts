/**
 * ゲーム名 → Steam アプリの解決とリリース判定 (spec/feature/flow/user-voices.md)。
 *
 * - ストア検索 (`/api/storesearch`) で候補を引き、候補名が議題に含まれる (または候補が名前を含む)
 *   ものだけを採用する (別ゲームの誤爆を避ける)。
 * - `appdetails` の `release_date.coming_soon` でリリース済みかを判定する。
 * API キー不要。ネットワーク I/O は fetchImpl で差し替え可能 (テスト)。
 */

const STORE = "https://store.steampowered.com";
const USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";

export interface SteamApp {
  appId: number;
  name: string;
  released: boolean;
}

/** 比較用の正規化 (NFKC・小文字・空白と記号の除去)。 */
export function normalizeGameName(value: string): string {
  return value
    .normalize("NFKC")
    .toLowerCase()
    .replace(/[\s　・:：\-–—_!！?？'"「」『』()（）[\]【】]/g, "");
}

/** 候補名が議題/指定名と同じゲームを指すか (双方向の部分一致、短すぎる名前は除外)。 */
export function isSameGame(candidate: string, query: string): boolean {
  const c = normalizeGameName(candidate);
  const q = normalizeGameName(query);
  if (c.length < 2 || q.length < 2) return false;
  return q.includes(c) || c.includes(q);
}

interface StoreSearchResponse {
  items?: Array<{ id?: number; name?: string }>;
}

interface AppDetailsResponse {
  [appId: string]: { success?: boolean; data?: { name?: string; release_date?: { coming_soon?: boolean } } };
}

async function getJson<T>(url: string, fetchImpl: typeof fetch): Promise<T | null> {
  const res = await fetchImpl(url, { headers: { "User-Agent": USER_AGENT, Accept: "application/json" } });
  if (!res.ok) return null;
  return (await res.json()) as T;
}

/**
 * 検索語の候補から Steam アプリを 1 つ解決する。見つからなければ null。
 * @param terms 検索に使う語 (指定ゲーム名、または議題から取り出した語)。先頭から順に試す。
 * @param matchAgainst 採用判定に使う文字列 (議題全文 or 指定ゲーム名)。
 */
export async function resolveSteamApp(args: {
  terms: readonly string[];
  matchAgainst: string;
  fetchImpl?: typeof fetch;
  maxQueries?: number;
}): Promise<SteamApp | null> {
  const fetchImpl = args.fetchImpl ?? fetch;
  const tried = new Set<string>();
  for (const term of args.terms) {
    const t = term.trim();
    if (!t || tried.has(t) || tried.size >= (args.maxQueries ?? 3)) continue;
    tried.add(t);
    const search = await getJson<StoreSearchResponse>(
      `${STORE}/api/storesearch/?term=${encodeURIComponent(t)}&l=japanese&cc=JP`,
      fetchImpl
    );
    const hit = (search?.items ?? []).find(
      (item) => typeof item.id === "number" && typeof item.name === "string" && isSameGame(item.name, args.matchAgainst)
    );
    if (!hit) continue;
    const details = await getJson<AppDetailsResponse>(
      `${STORE}/api/appdetails?appids=${hit.id}&cc=jp&l=japanese`,
      fetchImpl
    );
    const entry = details?.[String(hit.id)];
    if (!entry?.success) continue;
    return {
      appId: hit.id!,
      name: entry.data?.name ?? hit.name!,
      released: entry.data?.release_date?.coming_soon === false,
    };
  }
  return null;
}
