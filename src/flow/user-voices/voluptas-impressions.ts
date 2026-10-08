/**
 * Voluptas の「遊んだ感想」書き出し口 (`GET /api/personas/impressions`) の client
 * (Voluptas spec/feature/glab-impression-export.md, SPEC-GLAB-IMPRESSION-EXPORT)。
 *
 * Steam に無いゲームのユーザーの声として使う。感想には書き手の情報が無い (Voluptas 側で落とす)
 * ので、authorId は匿名の固定値にする。認証はペルソナ取り込みと同じ Bearer
 * (Cernere service token / 移行期間の固定トークン、resolveVoluptasBearer)。
 */

import type { ExternalUtterance } from "../../crawler/sources/types.js";

export class VoluptasUnavailable extends Error {
  constructor() { super("Voluptas（Vo）が停止中、または接続できません。"); }
}

/** 感想の書き手は特定しない (Voluptas は書き手の情報を返さない)。 */
export const GLAB_ANONYMOUS_AUTHOR = "anonymous";

export interface VoluptasImpression {
  id: string;
  gameTitle: string;
  recommend?: boolean | null;
  polarity?: string | null;
  comment: string;
  createdAt: string;
}

/** 感想 1 件 → ExternalUtterance (純関数)。 */
export function mapImpression(gameSlug: string, impression: VoluptasImpression): ExternalUtterance {
  const postedAt = Date.parse(impression.createdAt);
  return {
    source: "glab",
    nativeId: impression.id,
    gameSlug,
    threadKey: impression.gameTitle,
    content: impression.comment,
    postedAt: Number.isFinite(postedAt) ? postedAt : Date.now(),
    authorId: GLAB_ANONYMOUS_AUTHOR,
    signal: typeof impression.recommend === "boolean" ? { votedUp: impression.recommend } : undefined,
    // 出所の識別子 (公開 URL は無い)。書き手の情報は含まない。
    sourceUrl: `glab://impressions/${encodeURIComponent(impression.id)}`,
  };
}

function isImpression(v: unknown): v is VoluptasImpression {
  if (!v || typeof v !== "object") return false;
  const o = v as Record<string, unknown>;
  return typeof o.id === "string" && typeof o.gameTitle === "string" && typeof o.comment === "string" && o.comment.trim() !== "";
}

/**
 * ゲーム名で Voluptas の感想を取得する。
 * @param baseUrl Voluptas の origin (例 http://localhost:3015)。
 */
export async function fetchVoluptasImpressions(args: {
  baseUrl: string;
  bearer: string;
  game: string;
  gameSlug: string;
  limit: number;
  fetchImpl?: typeof fetch;
}): Promise<ExternalUtterance[]> {
  const url = new URL("/api/personas/impressions", args.baseUrl);
  if (url.protocol !== "https:" && !["localhost", "127.0.0.1", "[::1]"].includes(url.hostname)) {
    throw new Error("Voluptas URL must use HTTPS (HTTP is allowed only on loopback)");
  }
  url.searchParams.set("game", args.game);
  url.searchParams.set("limit", String(Math.max(1, Math.min(args.limit, 200))));
  let res: Response;
  try {
    res = await (args.fetchImpl ?? fetch)(url, {
      redirect: "manual", signal: AbortSignal.timeout(10000),
      headers: { Authorization: `Bearer ${args.bearer}`, Accept: "application/json" },
    });
  } catch { throw new VoluptasUnavailable(); }
  if (res.status >= 500) throw new VoluptasUnavailable();
  if (!res.ok) throw new Error(`Voluptas impressions request failed with status ${res.status}`);
  const body = (await res.json()) as { data?: { impressions?: unknown[] } };
  return (body.data?.impressions ?? []).filter(isImpression).map((i) => mapImpression(args.gameSlug, i));
}
