/**
 * ストアトレンド調査シード (#65)。
 *
 * Steam の公開 featuredcategories から「トップセラー」を取得し、 議論の種に変換する。
 * - API キー不要 (公開エンドポイント)。
 * - 取得失敗時は静的フォールバックで止めない (運用を継続)。
 * - App Store / Google Play は公開 API が薄く要調査 (task #65 メモ通り)、 当面 Steam 中心。
 */

import type { SeedTopic } from "./genres.js";

const STEAM_FEATURED_URL =
  "https://store.steampowered.com/api/featuredcategories/?cc=jp&l=japanese";

/** 取得失敗時の静的フォールバック (代表的な話題ジャンル/作品を種にする)。 */
const FALLBACK_TITLES: readonly string[] = [
  "オープンワールドサバイバルクラフト",
  "協力型ローグライト",
  "オートチェス / オートバトラー",
  "ライフシム",
  "抽出シューター (extraction shooter)",
];

interface SteamFeaturedItem {
  name?: unknown;
}
interface SteamFeaturedCategory {
  items?: unknown;
}
interface SteamFeaturedResponse {
  top_sellers?: SteamFeaturedCategory;
}

/** Steam トップセラーの作品名を取得 (失敗時は空配列)。 */
async function fetchSteamTopSellerNames(limit: number, timeoutMs = 8000): Promise<string[]> {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    const res = await fetch(STEAM_FEATURED_URL, { signal: ctrl.signal });
    if (!res.ok) return [];
    const json = (await res.json()) as SteamFeaturedResponse;
    const items = Array.isArray(json.top_sellers?.items) ? json.top_sellers!.items : [];
    const names: string[] = [];
    for (const raw of items as SteamFeaturedItem[]) {
      const name = typeof raw?.name === "string" ? raw.name.trim() : "";
      if (name) names.push(name);
      if (names.length >= limit) break;
    }
    return names;
  } catch {
    return [];
  } finally {
    clearTimeout(timer);
  }
}

function toTopic(title: string): SeedTopic {
  return {
    title: `【話題】${title}: 何が支持されている?`,
    description:
      `ストアで注目されている「${title}」。 プレイヤーに支持されている要素や、 議論されている点を` +
      ` 多角的に掘り下げ、 面白さ・課題・流行の理由を整理する。`,
  };
}

/**
 * ストアトレンドの種を取得する。 Steam 取得が空ならフォールバックを使う。
 * 必ず 1 件以上返す (= 自動シードが空振りしない)。
 */
export async function fetchStoreTrendTopics(limit = 5): Promise<SeedTopic[]> {
  const names = await fetchSteamTopSellerNames(limit);
  const source = names.length > 0 ? names : FALLBACK_TITLES.slice(0, limit);
  return source.map(toTopic);
}
