/**
 * Website (Web 記事) collector — spec/crawler/EXTERNAL-SOURCES.md §4.5。
 *
 * 任意の URL (レビュー / 考察ブログ / ニュース記事) を 1 件取得し、 本文を抽出して
 * 1 つの ExternalUtterance に正規化する。 記事 = ひとりの論者の 1 意見として扱う。
 * API キー不要 (公開 HTML を直接取得)。
 *
 * `extractArticle` / `mapWebsiteArticle` / `normalizeUrl` は純粋関数 (テスト用)、
 * `fetchWebsiteArticle` がネットワーク I/O。
 *
 * 抽出は依存ライブラリを足さない軽量ヒューリスティック (script/style 除去 →
 * <article>/<main> 優先 → タグ除去 → エンティティ復号 → 空白圧縮)。 完璧な
 * readability ではないが「粗く動かす」段階の取り込みには十分。
 */

import { setTimeout as sleep } from "node:timers/promises";

import type { ExternalUtterance } from "./types.js";

const SOURCE = "website" as const;
const USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";
/** 1 記事の本文上限 (議論シードに十分かつ巨大ページの暴発を防ぐ)。 */
const MAX_CONTENT_CHARS = 8000;

export interface ExtractedArticle {
  title: string;
  /** 抽出した本文 (タグ除去・空白圧縮済み)。 */
  text: string;
  /** 署名 (meta author 等)。 取れなければ undefined。 */
  author?: string;
}

/** URL を dedup / アンカー用に正規化 (hash 除去 + tracking パラメータ除去)。 */
export function normalizeUrl(rawUrl: string): string {
  let u: URL;
  try {
    u = new URL(rawUrl);
  } catch {
    return rawUrl.trim();
  }
  u.hash = "";
  const drop: string[] = [];
  u.searchParams.forEach((_v, k) => {
    if (/^(utm_|fbclid$|gclid$|yclid$|mc_eid$|ref$|ref_src$)/i.test(k)) drop.push(k);
  });
  for (const k of drop) u.searchParams.delete(k);
  // 末尾スラッシュは保持しつつ、 空クエリの "?" を落とす
  return u.toString().replace(/\?$/, "");
}

function decodeEntities(s: string): string {
  return s
    .replace(/&nbsp;/g, " ")
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"')
    .replace(/&#39;|&apos;/g, "'")
    .replace(/&#(\d+);/g, (_m, d) => String.fromCodePoint(Number(d)))
    .replace(/&#x([0-9a-f]+);/gi, (_m, h) => String.fromCodePoint(parseInt(h, 16)));
}

function stripTags(html: string): string {
  return decodeEntities(
    html
      .replace(/<br\s*\/?>(?=)/gi, "\n")
      .replace(/<\/(p|div|li|h[1-6]|section|article)>/gi, "\n")
      .replace(/<[^>]+>/g, " ")
  )
    .replace(/[ \t ]+/g, " ")
    .replace(/\s*\n\s*/g, "\n")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
}

function firstMatch(html: string, re: RegExp): string | undefined {
  const m = html.match(re);
  return m?.[1]?.trim() || undefined;
}

/** 生 HTML → {title, text, author} を抽出 (純粋関数)。 */
export function extractArticle(html: string): ExtractedArticle {
  // 本文に効かない領域を除去
  const cleaned = html
    .replace(/<!--[\s\S]*?-->/g, " ")
    .replace(/<script[\s\S]*?<\/script>/gi, " ")
    .replace(/<style[\s\S]*?<\/style>/gi, " ")
    .replace(/<noscript[\s\S]*?<\/noscript>/gi, " ")
    .replace(/<(nav|header|footer|aside|form)[\s\S]*?<\/\1>/gi, " ");

  const title =
    firstMatch(html, /<meta[^>]+property=["']og:title["'][^>]+content=["']([^"']+)["']/i) ??
    firstMatch(html, /<title[^>]*>([\s\S]*?)<\/title>/i) ??
    firstMatch(cleaned, /<h1[^>]*>([\s\S]*?)<\/h1>/i) ??
    "";

  const author =
    firstMatch(html, /<meta[^>]+name=["']author["'][^>]+content=["']([^"']+)["']/i) ??
    firstMatch(html, /<meta[^>]+property=["']article:author["'][^>]+content=["']([^"']+)["']/i);

  // 本文候補: <article> → <main> → <body> の順で最初に見つかったもの
  const region =
    firstMatch(cleaned, /<article[^>]*>([\s\S]*?)<\/article>/i) ??
    firstMatch(cleaned, /<main[^>]*>([\s\S]*?)<\/main>/i) ??
    firstMatch(cleaned, /<body[^>]*>([\s\S]*?)<\/body>/i) ??
    cleaned;

  let text = stripTags(region);
  if (text.length > MAX_CONTENT_CHARS) text = `${text.slice(0, MAX_CONTENT_CHARS - 1)}…`;

  return { title: decodeEntities(stripTags(title)).trim(), text, author: author ? decodeEntities(author).trim() : undefined };
}

function detectLang(text: string): string {
  return /[぀-ヿ㐀-鿿]/.test(text) ? "ja" : "en";
}

export interface WebsiteMapInput {
  url: string;
  gameSlug: string;
  fetchedAt: number;
  article: ExtractedArticle;
}

/** 抽出済み記事 → ExternalUtterance (純粋関数)。 1 記事 = 1 発話 (= 1 論者の意見)。 */
export function mapWebsiteArticle(input: WebsiteMapInput): ExternalUtterance {
  const canonical = normalizeUrl(input.url);
  const host = (() => {
    try {
      return new URL(canonical).hostname.replace(/^www\./, "");
    } catch {
      return "web";
    }
  })();
  const body = input.article.text;
  const content = input.article.title ? `${input.article.title}\n\n${body}` : body;
  return {
    source: SOURCE,
    nativeId: canonical, // URL を一意 id に (dedup key)
    gameSlug: input.gameSlug,
    threadKey: canonical, // 記事 1 本 = 1 スレッド (session)
    content,
    lang: detectLang(content),
    postedAt: input.fetchedAt,
    // 公開・安定な同一性アンカー: 署名があれば「サイト:著者」、 無ければサイトドメイン
    authorId: input.article.author ? `${host}:${input.article.author}` : host,
    authorName: input.article.author ?? host,
    sourceUrl: canonical,
  };
}

export interface WebsiteFetchOptions {
  /** 取得対象 URL 群 */
  urls: string[];
  gameSlug: string;
  /** 同一ドメインへの最小間隔 ms。 既定 1500 */
  politeDelayMs?: number;
  /** DI 用。 既定 global fetch */
  fetchImpl?: typeof fetch;
  /** 「取得時刻」 の注入 (テスト決定性用)。 既定 Date.now() */
  now?: () => number;
  /** 1 件取得ごとの進捗コールバック (累計件数) */
  onPage?: (total: number) => void;
  /** 取得失敗時のハンドラ (既定: stderr へ警告し継続) */
  onError?: (url: string, err: unknown) => void;
}

/** URL 群を順に取得し、 ExternalUtterance[] に正規化する (失敗 URL は skip)。 */
export async function fetchWebsiteArticles(opts: WebsiteFetchOptions): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const now = opts.now ?? (() => Date.now());
  const delay = opts.politeDelayMs ?? 1500;
  const onError =
    opts.onError ?? ((url, err) => process.stderr.write(`\n  skip ${url}: ${(err as Error).message}\n`));

  const out: ExternalUtterance[] = [];
  for (let i = 0; i < opts.urls.length; i += 1) {
    const url = opts.urls[i];
    try {
      const res = await doFetch(url, { headers: { "User-Agent": USER_AGENT, Accept: "text/html" } });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const html = await res.text();
      const article = extractArticle(html);
      if (!article.text || article.text.length < 80) throw new Error("本文抽出が短すぎ (非記事ページ?)");
      out.push(mapWebsiteArticle({ url, gameSlug: opts.gameSlug, fetchedAt: now(), article }));
      opts.onPage?.(out.length);
    } catch (err) {
      onError(url, err);
    }
    if (i < opts.urls.length - 1) await sleep(delay);
  }
  return out;
}
