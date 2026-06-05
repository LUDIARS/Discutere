/**
 * Fandom (旧 Wikia) collector (Phase 1) — spec/crawler/EXTERNAL-SOURCES.md §「5 Fandom」。
 *
 * MediaWiki API (`action=parse`) で wiki 記事本文を取得し、HTML を素朴に plaintext 化して
 * ExternalUtterance に正規化する。API キー不要 (CC-BY-SA)。記事=ゲームの lore/設計文脈 (Axis1
 * 寄り) なので長文 → importer 側で要約/raw の 2 層化を使う。
 *
 * `mapFandomPage` は純粋関数 (テスト用)、`fetchFandomPages` がネットワーク I/O。
 */

import { setTimeout as sleep } from "node:timers/promises";

import type { ExternalUtterance } from "./types.js";

const SOURCE = "fandom" as const;
const USER_AGENT = "LUDIARS-Discutere-Crawler/0.1 (+https://github.com/LUDIARS)";

/** MediaWiki action=parse の応答 (必要分のみ)。 */
export interface FandomParseRaw {
  pageid: number;
  title: string;
  /** prop=text の HTML 本文 */
  html: string;
  /** ISO 文字列。無ければ未設定 */
  touched?: string;
}

/** ごく素朴な HTML→plaintext。MediaWiki のパーサ出力 (.mw-parser-output) を読める形に落とす。 */
export function htmlToText(html: string): string {
  return html
    .replace(/<script[\s\S]*?<\/script>/gi, " ")
    .replace(/<style[\s\S]*?<\/style>/gi, " ")
    .replace(/<sup[^>]*class="[^"]*reference[^"]*"[\s\S]*?<\/sup>/gi, " ") // [1] 等の脚注
    .replace(/<[^>]+>/g, " ")
    .replace(/&nbsp;/g, " ")
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"')
    .replace(/&#39;|&apos;/g, "'")
    .replace(/\[\d+\]/g, "") // 脚注番号
    .replace(/[ \t]+/g, " ")
    .replace(/\s*\n\s*/g, "\n")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
}

/** Fandom 記事 1 ページ → ExternalUtterance (純粋関数)。 */
export function mapFandomPage(host: string, gameSlug: string, raw: FandomParseRaw): ExternalUtterance {
  const slugTitle = raw.title.replace(/ /g, "_");
  return {
    source: SOURCE,
    nativeId: `${host}#${raw.pageid}`,
    gameSlug,
    threadKey: host, // wiki 単位を「議論の場」(session) とみなす
    content: htmlToText(raw.html),
    lang: host.startsWith("ja.") ? "ja" : "en",
    postedAt: raw.touched ? Date.parse(raw.touched) : 0,
    authorId: `wiki:${host}`, // wiki = 集合著者。公開・安定アンカー (spec §6)
    authorName: host,
    sourceUrl: `https://${host}/wiki/${encodeURIComponent(slugTitle)}`,
  };
}

export interface FandomFetchOptions {
  /** wiki ホスト名。例 "journey.fandom.com" / "deathstranding.fandom.com" */
  host: string;
  gameSlug: string;
  /** 取得するページタイトル群 */
  titles: string[];
  /** 同一ホストへの最小間隔 ms。既定 1500 */
  politeDelayMs?: number;
  fetchImpl?: typeof fetch;
  onPage?: (total: number) => void;
}

interface ParseApiResponse {
  parse?: { pageid: number; title: string; text: string; revid?: number };
  error?: { code: string; info: string };
}

/** 指定 wiki の指定ページ群を MediaWiki API で取得して正規化する。 */
export async function fetchFandomPages(opts: FandomFetchOptions): Promise<ExternalUtterance[]> {
  const doFetch = opts.fetchImpl ?? fetch;
  const delay = opts.politeDelayMs ?? 1500;
  const out: ExternalUtterance[] = [];

  for (const title of opts.titles) {
    const url =
      `https://${opts.host}/api.php?action=parse&format=json&formatversion=2&redirects=1` +
      `&prop=text&disablelimitreport=1&disableeditsection=1&page=${encodeURIComponent(title)}`;
    const res = await doFetch(url, { headers: { "User-Agent": USER_AGENT, Accept: "application/json" } });
    if (!res.ok) throw new Error(`fandom api HTTP ${res.status} (${opts.host}/${title})`);
    const data = (await res.json()) as ParseApiResponse;
    if (data.error || !data.parse) {
      // ページ欠落は skip (致命にしない)
      continue;
    }
    out.push(
      mapFandomPage(opts.host, opts.gameSlug, {
        pageid: data.parse.pageid,
        title: data.parse.title,
        html: data.parse.text,
      })
    );
    opts.onPage?.(out.length);
    await sleep(delay);
  }

  return out;
}
