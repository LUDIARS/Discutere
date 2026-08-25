import { timingSafeEqual } from "node:crypto";

import { Hono } from "hono";

import {
  loadStoredSteamReviewTrends,
  type GameReviewTrend,
} from "../integrations/glab-review-trends.js";

type TrendLoader = () => GameReviewTrend[];
type TokenLoader = () => string;

const MIN_TOKEN_LENGTH = 32;

function bearerToken(header: string | undefined): string {
  const match = /^Bearer ([^\s]+)$/.exec(header ?? "");
  return match?.[1] ?? "";
}

function secureMatch(actual: string, expected: string): boolean {
  const left = Buffer.from(actual);
  const right = Buffer.from(expected);
  return left.length === right.length && timingSafeEqual(left, right);
}

/** GLAB には匿名化済みのゲーム別集計だけを公開する。 */
export function createGlabReviewTrendRoutes(
  loadTrends: TrendLoader = loadStoredSteamReviewTrends,
  token: TokenLoader = () => process.env.DISCUTERE_GLAB_REVIEW_TRENDS_TOKEN ?? "",
): Hono {
  const routes = new Hono();
  routes.get("/integrations/glab/review-trends", (c) => {
    c.header("cache-control", "private, no-store");
    const expected = token();
    if (expected.length < MIN_TOKEN_LENGTH) {
      return c.json({ ok: false, error: "review_trends_unavailable" }, 503);
    }
    if (!secureMatch(bearerToken(c.req.header("authorization")), expected)) {
      return c.json({ ok: false, error: "unauthorized" }, 401);
    }
    try {
      return c.json({
        ok: true,
        source: "discutere",
        windowDays: 7,
        data: loadTrends(),
      });
    } catch {
      return c.json({ ok: false, error: "review_trends_unavailable" }, 503);
    }
  });
  return routes;
}

export const glabReviewTrendRoutes = createGlabReviewTrendRoutes();
