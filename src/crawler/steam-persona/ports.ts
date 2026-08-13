/**
 * steam-persona が使う取得層 (①Canalis steam adapter) の注入境界。
 *
 * 実体は @ludiars/canalis/steam の fetchNewReleases / fetchAppReviewSummary /
 * fetchAppReviews / fetchUserReviews。テストではここを fake に差し替えるため、
 * 必要な形だけを構造的に写した型を置く (Canalis 側の型変更には typecheck で気付く)。
 */

/** 新作 1 件 (Canalis SteamSearchEntry と構造互換)。 */
export interface NewReleaseEntry {
  appId: number;
  title: string;
  url: string;
  releaseText?: string;
}

/** アプリ別レビュー 1 件 (Canalis SteamAppReviewRaw と構造互換、必要フィールドのみ)。 */
export interface AppReviewEntry {
  recommendationid: string;
  author?: { steamid?: string };
  review: string;
  language?: string;
  timestamp_created: number;
  voted_up: boolean;
  votes_up?: number;
}

/** ユーザ別レビュー 1 件 (Canalis SteamUserReviewEntry と構造互換)。 */
export interface UserReviewEntry {
  steamId: string;
  appId: number;
  recommended?: boolean;
  text: string;
  hoursText?: string;
  postedText?: string;
  url: string;
}

/** @implements SPEC-STEAM-PERSONA-PIPELINE — 公開プロフィール URL に使える SteamID64 形式だけを受け入れる。 */
export function isSteamId64(value: string): boolean {
  return /^\d{17}$/.test(value);
}

/** 取得層の注入 interface。runner が Canalis 実装を束ね、テストは fake を渡す。 */
export interface SteamFetchPorts {
  fetchNewReleases(opts: { maxApps: number }): Promise<NewReleaseEntry[]>;
  fetchTotalReviews(appId: number): Promise<number>;
  fetchAppReviews(opts: {
    appId: number;
    maxReviews: number;
    stopAtRecommendationId?: string;
  }): Promise<AppReviewEntry[]>;
  fetchUserReviews(opts: { steamId: string }): Promise<UserReviewEntry[]>;
}
