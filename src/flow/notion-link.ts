/**
 * ペーパー作成の入力に含まれる **Notion リンクを Canalis でクロール**して本文 md に展開する。
 *
 * - token (env NOTION_TOKEN) があれば Canalis `crawlPage` で API から取得し、子ページを深さ N まで辿る。
 * - token が無ければ Canalis `NotionPublicSource` で公開ページをブラウザ取得する (起点ページのみ)。
 * - 深さは既定 `flow.notionLinks.maxDepth` (2)。本文の「深さ:N」/「depth:N」指示で上書きできる。
 *
 * 取得失敗は warn して他リンク・議論を止めない (graceful)。Notion API / ブラウザは注入境界。
 */

import {
  NotionApiClient,
  NotionPublicSource,
  crawlPage,
  findNotionUrls,
  parseNotionPageId,
  type NotionApi,
} from "@ludiars/canalis";
import { getConfig } from "../config.js";

/** 深さ指示の上限 (全階層取得の暴発を防ぐ)。 */
export const MAX_NOTION_DEPTH = 5;

const DEPTH_RE = /(?:深さ|depth)\s*[:：=]\s*(\d+)/i;

/** 公開ページ取得 (token 無し時)。 */
export type NotionPublicFetch = (url: string) => Promise<{ title?: string; markdown: string }>;

export interface NotionLinkDeps {
  /** Notion API。null なら公開ページ取得へ回す。省略時は env NOTION_TOKEN から生成。 */
  api?: NotionApi | null;
  /** 公開ページ取得。省略時は Canalis NotionPublicSource。 */
  fetchPublic?: NotionPublicFetch;
}

export interface ExpandNotionOpts {
  /** 深さ (起点=0)。省略時は本文の深さ指示 → config 既定。 */
  depth?: number;
  /** 1 リンクあたりの最大ページ数。省略時は config 既定。 */
  maxPages?: number;
  deps?: NotionLinkDeps;
  warn?: (msg: string) => void;
}

/** 本文中の「深さ:N」指示を読む (0..MAX_NOTION_DEPTH にクランプ)。無ければ undefined。 */
export function parseNotionDepth(text: string): number | undefined {
  const m = DEPTH_RE.exec(text);
  if (!m) return undefined;
  return Math.min(MAX_NOTION_DEPTH, Math.max(0, Number(m[1])));
}

function defaultApi(): NotionApi | null {
  const token = process.env.NOTION_TOKEN?.trim();
  return token ? new NotionApiClient({ token, notionVersion: process.env.NOTION_VERSION }) : null;
}

const defaultFetchPublic: NotionPublicFetch = async (url) => {
  const [rec] = await new NotionPublicSource().crawl({ url });
  return { title: rec?.title, markdown: rec?.text ?? "" };
};

/** 1 リンクを md に解決する。API 経由は子ページを見出し付きで連結する。 */
export async function resolveNotionUrl(
  url: string,
  opts: { depth: number; maxPages: number; deps?: NotionLinkDeps; warn?: (msg: string) => void }
): Promise<string> {
  const api = opts.deps?.api !== undefined ? opts.deps.api : defaultApi();
  const pageId = parseNotionPageId(url);
  if (api && pageId) {
    const r = await crawlPage(api, pageId, { maxDepth: opts.depth, maxPages: opts.maxPages });
    for (const e of r.errors) opts.warn?.(`Notion ${e.stage} 失敗 (${e.id}): ${e.message}`);
    if (r.truncated) opts.warn?.(`Notion ページ数上限 ${opts.maxPages} で打ち切り (${url})`);
    if (r.pages.length > 0) {
      return r.pages
        .map((p) => `${"#".repeat(Math.min(6, 2 + p.depth))} ${p.title || "(無題)"}\n出典: ${p.url || url}\n\n${p.markdown}`.trim())
        .join("\n\n");
    }
    opts.warn?.(`Notion API で取得できず公開ページ取得へ切替 (${url})`);
  }
  const page = await (opts.deps?.fetchPublic ?? defaultFetchPublic)(url);
  return `## ${page.title || "(無題)"}\n出典: ${url}\n\n${page.markdown}`.trim();
}

/**
 * 本文中の Notion リンクを全てクロールし、`# Notion 資料` 節の md にまとめて返す。
 * リンクが無い / 全件失敗なら ""。
 */
export async function expandNotionLinks(text: string, opts: ExpandNotionOpts = {}): Promise<string> {
  const urls = findNotionUrls(text);
  if (urls.length === 0) return "";
  const cfg = getConfig().flow.notionLinks;
  const depth = opts.depth ?? parseNotionDepth(text) ?? cfg.maxDepth;
  const maxPages = opts.maxPages ?? cfg.maxPages;
  const parts: string[] = [];
  for (const url of urls) {
    try {
      const md = await resolveNotionUrl(url, { depth, maxPages, deps: opts.deps, warn: opts.warn });
      if (md) parts.push(md);
    } catch (e) {
      opts.warn?.(`Notion リンク取得失敗 (${url}): ${(e as Error).message}`);
    }
  }
  return parts.length > 0 ? `# Notion 資料\n\n${parts.join("\n\n")}` : "";
}
