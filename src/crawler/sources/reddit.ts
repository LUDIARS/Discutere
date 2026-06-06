/**
 * Reddit collector (Phase 2) — spec/crawler/EXTERNAL-SOURCES.md §4.3。
 *
 * OAuth2 (script app, client_credentials grant) で app-only token を取り、
 * subreddit / 全体検索でスレッドを漁り、 各スレッドのコメントツリーを
 * ExternalUtterance に正規化する。 username は公開・安定な persona アンカー。
 *
 * `mapRedditComment` / `flattenComments` は純粋関数 (テスト用)、 他はネットワーク I/O。
 * 認証情報は呼び出し側が env から渡す (config に平文で置かない)。
 */

import { setTimeout as sleep } from "node:timers/promises";

import type { ExternalUtterance } from "./types.js";

const SOURCE = "reddit" as const;
const OAUTH_BASE = "https://oauth.reddit.com";
const TOKEN_URL = "https://www.reddit.com/api/v1/access_token";

export interface RedditCredentials {
  clientId: string;
  clientSecret: string;
  /** Reddit 必須の User-Agent (例 "LUDIARS-Discutere/0.1 by /u/<acct>") */
  userAgent: string;
}

export interface RedditThreadRef {
  /** submission id (t3 の base36、 例 "abc123") */
  id: string;
  title: string;
  subreddit: string;
  permalink: string;
  numComments: number;
}

// ── pure mappers ─────────────────────────────────────────────

export interface RedditCommentRaw {
  name?: string; // "t1_xxxx"
  id?: string;
  body?: string;
  author?: string;
  created_utc?: number; // epoch seconds
  parent_id?: string; // "t3_<sub>" (top-level) or "t1_<id>" (reply)
  ups?: number;
  replies?: unknown;
}

/** 削除済 / 空コメントは取り込まない。 */
function isDroppableBody(body: string | undefined): boolean {
  if (!body) return true;
  const t = body.trim();
  return t.length === 0 || t === "[deleted]" || t === "[removed]";
}

/** Reddit comment 1 件 → ExternalUtterance (純粋関数)。 取り込み対象外なら null。 */
export function mapRedditComment(
  raw: RedditCommentRaw,
  ctx: { submissionId: string; gameSlug: string; permalink?: string }
): ExternalUtterance | null {
  if (isDroppableBody(raw.body)) return null;
  const nativeId = raw.name ?? (raw.id ? `t1_${raw.id}` : "");
  if (!nativeId) return null;
  const author = raw.author && raw.author !== "[deleted]" ? raw.author : "anonymous";
  // 親が submission (t3_) ならスレッド直下 → responds_to 無し。 t1_ なら親コメント。
  const parentNativeId = raw.parent_id?.startsWith("t1_") ? raw.parent_id : undefined;
  return {
    source: SOURCE,
    nativeId,
    gameSlug: ctx.gameSlug,
    threadKey: ctx.submissionId,
    content: raw.body ?? "",
    postedAt: (raw.created_utc ?? 0) * 1000,
    authorId: author, // Reddit username = 公開・安定アンカー (§6)
    authorName: author,
    parentNativeId,
    signal: { upvotes: raw.ups },
    sourceUrl: ctx.permalink
      ? `https://www.reddit.com${ctx.permalink}${(raw.id ?? "").replace(/^t1_/, "")}/`
      : `https://www.reddit.com/comments/${ctx.submissionId}/`,
  };
}

interface ListingNode {
  kind?: string;
  data?: RedditCommentRaw & { children?: ListingNode[] };
}

/** コメントツリー (listing children) を深さ優先で平坦化 (max まで)。 */
export function flattenComments(
  children: ListingNode[],
  ctx: { submissionId: string; gameSlug: string; permalink?: string },
  max: number
): ExternalUtterance[] {
  const out: ExternalUtterance[] = [];
  const stack = [...children];
  while (stack.length > 0 && out.length < max) {
    const node = stack.shift();
    if (!node || node.kind !== "t1" || !node.data) continue;
    const mapped = mapRedditComment(node.data, ctx);
    if (mapped) out.push(mapped);
    // 返信 (replies) は listing or "" 。 listing なら children を積む。
    const replies = node.data.replies;
    if (replies && typeof replies === "object") {
      const repChildren = (replies as { data?: { children?: ListingNode[] } }).data?.children;
      if (Array.isArray(repChildren)) stack.push(...repChildren);
    }
  }
  return out;
}

// ── network ──────────────────────────────────────────────────

async function getJson(url: string, headers: Record<string, string>, fetchImpl: typeof fetch): Promise<any> {
  const res = await fetchImpl(url, { headers });
  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`reddit ${res.status}: ${body.slice(0, 200)}`);
  }
  return res.json();
}

/** client_credentials grant で app-only access token を取得。 */
export async function getRedditToken(
  creds: RedditCredentials,
  fetchImpl: typeof fetch = fetch
): Promise<string> {
  const basic = Buffer.from(`${creds.clientId}:${creds.clientSecret}`).toString("base64");
  const res = await fetchImpl(TOKEN_URL, {
    method: "POST",
    headers: {
      Authorization: `Basic ${basic}`,
      "Content-Type": "application/x-www-form-urlencoded",
      "User-Agent": creds.userAgent,
    },
    body: "grant_type=client_credentials",
  });
  if (!res.ok) throw new Error(`reddit token HTTP ${res.status}`);
  const data = (await res.json()) as { access_token?: string };
  if (!data.access_token) throw new Error("reddit token: access_token 欠落");
  return data.access_token;
}

export interface RedditSearchOptions {
  token: string;
  userAgent: string;
  query: string;
  /** subreddit に限定する場合 (例 "vampiresurvivors")。 未指定なら全体検索 */
  subreddit?: string;
  /** relevance | top | new | comments。 既定 "top" */
  sort?: string;
  /** hour | day | week | month | year | all。 既定 "year" */
  time?: string;
  /** 取得スレッド上限 (1-100)。 既定 25 */
  limit?: number;
  fetchImpl?: typeof fetch;
}

/** スレッドを検索する (t3 listing → RedditThreadRef[])。 */
export async function searchThreads(opts: RedditSearchOptions): Promise<RedditThreadRef[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const limit = Math.min(100, Math.max(1, opts.limit ?? 25));
  const sort = opts.sort ?? "top";
  const time = opts.time ?? "year";
  const path = opts.subreddit
    ? `/r/${encodeURIComponent(opts.subreddit)}/search`
    : `/search`;
  const restrict = opts.subreddit ? "&restrict_sr=1" : "";
  const url =
    `${OAUTH_BASE}${path}?q=${encodeURIComponent(opts.query)}` +
    `&sort=${sort}&t=${time}&limit=${limit}&type=link${restrict}&raw_json=1`;
  const data = await getJson(url, { Authorization: `Bearer ${opts.token}`, "User-Agent": opts.userAgent }, doFetch);
  const children = (data?.data?.children ?? []) as Array<{ data?: any }>;
  return children
    .map((c) => c.data)
    .filter((d) => d && typeof d.id === "string")
    .map((d) => ({
      id: d.id as string,
      title: (d.title as string) ?? "",
      subreddit: (d.subreddit as string) ?? "",
      permalink: (d.permalink as string) ?? `/comments/${d.id}/`,
      numComments: typeof d.num_comments === "number" ? d.num_comments : 0,
    }));
}

export interface RedditCommentsOptions {
  token: string;
  userAgent: string;
  submissionId: string;
  gameSlug: string;
  permalink?: string;
  /** コメント取得上限。 既定 500 */
  maxComments?: number;
  /** ツリー深さ (Reddit の depth param)。 既定 10 */
  depth?: number;
  fetchImpl?: typeof fetch;
}

/** 1 スレッドのコメントツリーを取得して平坦化する。 */
export async function fetchThreadComments(opts: RedditCommentsOptions): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const max = opts.maxComments ?? 500;
  const depth = opts.depth ?? 10;
  const url =
    `${OAUTH_BASE}/comments/${encodeURIComponent(opts.submissionId)}?depth=${depth}&limit=${max}&raw_json=1`;
  const data = (await getJson(url, { Authorization: `Bearer ${opts.token}`, "User-Agent": opts.userAgent }, doFetch)) as unknown;
  // data = [postListing, commentsListing]
  const commentsListing = Array.isArray(data) ? (data[1] as { data?: { children?: ListingNode[] } }) : undefined;
  const children = commentsListing?.data?.children ?? [];
  return flattenComments(children, { submissionId: opts.submissionId, gameSlug: opts.gameSlug, permalink: opts.permalink }, max);
}

export interface RedditFetchOptions extends RedditCredentials {
  gameSlug: string;
  query: string;
  subreddit?: string;
  /** 漁るスレッド数の上限。 既定 5 */
  maxThreads?: number;
  /** 1 スレッドあたりのコメント上限。 既定 300 */
  maxCommentsPerThread?: number;
  /** スレッド間の最小間隔 ms (rate 配慮)。 既定 1500 */
  politeDelayMs?: number;
  fetchImpl?: typeof fetch;
  onThread?: (info: { title: string; comments: number; total: number }) => void;
}

/** 検索 → 上位スレッド → コメント を一括取得して ExternalUtterance[] を返す。 */
export async function fetchRedditDiscussions(opts: RedditFetchOptions): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const token = await getRedditToken(opts, doFetch);
  const maxThreads = opts.maxThreads ?? 5;
  const delay = opts.politeDelayMs ?? 1500;
  const threads = (
    await searchThreads({
      token,
      userAgent: opts.userAgent,
      query: opts.query,
      subreddit: opts.subreddit,
      limit: maxThreads,
      fetchImpl: doFetch,
    })
  ).slice(0, maxThreads);

  const out: ExternalUtterance[] = [];
  for (let i = 0; i < threads.length; i += 1) {
    const t = threads[i];
    const comments = await fetchThreadComments({
      token,
      userAgent: opts.userAgent,
      submissionId: t.id,
      gameSlug: opts.gameSlug,
      permalink: t.permalink,
      maxComments: opts.maxCommentsPerThread ?? 300,
      fetchImpl: doFetch,
    });
    out.push(...comments);
    opts.onThread?.({ title: t.title, comments: comments.length, total: out.length });
    if (i < threads.length - 1) await sleep(delay);
  }
  return out;
}
