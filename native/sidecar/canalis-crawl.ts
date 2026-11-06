// Notion リンクのクロール (@ludiars/canalis) を Node のまま行うサイドカー。
// C++ (native/src/flow/notion_link.cpp) が引数配列で起動し、標準入力に JSON を 1 つ渡す:
//   { "url": "https://www.notion.so/...", "depth": 0..5, "maxPages": 1..200 }
// 移植前の src/flow/notion-link.ts の resolveNotionUrl と同じ手順で Canalis の関数を呼ぶ:
//   - env NOTION_TOKEN があれば NotionApiClient + crawlPage (子ページを depth まで)。取れなければ公開ページへ切替
//   - 無ければ crawlPublicPages (Playwright で公開ページを取得し、本文中のページリンクを depth まで)
// token は Node 側で env から読み、C++ は token を持たない。標準出力には
//   {"ok":true,"result":{"pages":[{url,title,depth,markdown}],"warnings":[...]}} か {"ok":false,"error":"..."} を 1 行だけ書く。
// ページの md への連結 (見出し・出典) は C++ 側で行う。
import { pathToFileURL } from "node:url";

/** C++ と同じ上限 (native/src/flow/notion_link.hpp)。 */
export const MAX_NOTION_DEPTH = 5;
export const MAX_NOTION_PAGES = 200;
const MAX_URL_LENGTH = 2048;
const MAX_INPUT_BYTES = 16 * 1024;

export interface CrawlInput {
  url: string;
  depth: number;
  maxPages: number;
}

export interface CrawledPage {
  url: string;
  title: string;
  depth: number;
  markdown: string;
}

export interface CrawlOutput {
  pages: CrawledPage[];
  warnings: string[];
}

type PageResult = { pages: CrawledPage[]; errors: Array<{ id: string; stage: string; message: string }>; truncated: boolean };
type PublicResult = { pages: CrawledPage[]; errors: Array<{ url: string; message: string }>; truncated: boolean };

/** Canalis の境界 (テストで差し替える)。api が null なら公開ページ取得のみ。 */
export interface CrawlDeps {
  api: unknown | null;
  isNotionUrl: (url: string) => boolean;
  parseNotionPageId: (url: string) => string | null;
  crawlPage: (api: unknown, pageId: string, opts: { maxDepth: number; maxPages: number }) => Promise<PageResult>;
  crawlPublic: (url: string, opts: { maxDepth: number; maxPages: number }) => Promise<PublicResult>;
}

function isInt(v: unknown, min: number, max: number): v is number {
  return typeof v === "number" && Number.isInteger(v) && v >= min && v <= max;
}

/** 入力の検査。通らなければ null。 */
export function parseCrawlInput(value: unknown, isNotionUrl: (url: string) => boolean): CrawlInput | null {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const o = value as Record<string, unknown>;
  if (typeof o.url !== "string" || o.url.length === 0 || o.url.length > MAX_URL_LENGTH || !isNotionUrl(o.url)) return null;
  if (!isInt(o.depth, 0, MAX_NOTION_DEPTH) || !isInt(o.maxPages, 1, MAX_NOTION_PAGES)) return null;
  return { url: o.url, depth: o.depth, maxPages: o.maxPages };
}

function pick(p: { url?: unknown; title?: unknown; depth?: unknown; markdown?: unknown }): CrawledPage {
  return {
    url: typeof p.url === "string" ? p.url : "",
    title: typeof p.title === "string" ? p.title : "",
    depth: typeof p.depth === "number" ? p.depth : 0,
    markdown: typeof p.markdown === "string" ? p.markdown : "",
  };
}

/** 1 リンクを取得する (移植前 resolveNotionUrl の取得部分。warn の文言も同じ)。 */
export async function crawlNotionUrl(input: CrawlInput, deps: CrawlDeps): Promise<CrawlOutput> {
  const warnings: string[] = [];
  const { url, depth, maxPages } = input;
  const pageId = deps.parseNotionPageId(url);
  if (deps.api && pageId) {
    const r = await deps.crawlPage(deps.api, pageId, { maxDepth: depth, maxPages });
    for (const e of r.errors) warnings.push(`Notion ${e.stage} 失敗 (${e.id}): ${e.message}`);
    if (r.truncated) warnings.push(`Notion ページ数上限 ${maxPages} で打ち切り (${url})`);
    if (r.pages.length > 0) return { pages: r.pages.map(pick), warnings };
    warnings.push(`Notion API で取得できず公開ページ取得へ切替 (${url})`);
  }
  const pub = await deps.crawlPublic(url, { maxDepth: depth, maxPages });
  for (const e of pub.errors) warnings.push(`Notion 公開ページ取得失敗 (${e.url}): ${e.message}`);
  if (pub.truncated) warnings.push(`Notion ページ数上限 ${maxPages} で打ち切り (${url})`);
  return { pages: pub.pages.map(pick), warnings };
}

function reply(value: unknown): void {
  process.stdout.write(`${JSON.stringify(value)}\n`);
}

async function readStdin(): Promise<string> {
  const chunks: Buffer[] = [];
  let size = 0;
  for await (const chunk of process.stdin) {
    const buf = Buffer.isBuffer(chunk) ? chunk : Buffer.from(String(chunk));
    size += buf.length;
    if (size > MAX_INPUT_BYTES) throw new Error("sidecar input too large");
    chunks.push(buf);
  }
  return Buffer.concat(chunks).toString("utf8");
}

async function main(): Promise<void> {
  // 依存ライブラリの console 出力で標準出力の JSON を壊さないよう、全部標準エラーへ回す。
  const toStderr = (...args: unknown[]) => process.stderr.write(`${args.map(String).join(" ")}\n`);
  console.log = toStderr;
  console.info = toStderr;
  console.warn = toStderr;
  console.debug = toStderr;

  const canalis = await import("@ludiars/canalis");
  let raw: unknown;
  try {
    raw = JSON.parse(await readStdin());
  } catch (error) {
    reply({ ok: false, error: `invalid sidecar input: ${(error as Error).message}` });
    return;
  }
  const input = parseCrawlInput(raw, canalis.isNotionUrl);
  if (!input) {
    reply({ ok: false, error: "invalid sidecar input" });
    return;
  }
  const token = process.env.NOTION_TOKEN?.trim();
  const deps: CrawlDeps = {
    api: token ? new canalis.NotionApiClient({ token, notionVersion: process.env.NOTION_VERSION }) : null,
    isNotionUrl: canalis.isNotionUrl,
    parseNotionPageId: canalis.parseNotionPageId,
    crawlPage: (api, pageId, opts) => canalis.crawlPage(api as never, pageId, opts) as Promise<PageResult>,
    crawlPublic: (url, opts) => canalis.crawlPublicPages(url, opts) as Promise<PublicResult>,
  };
  try {
    reply({ ok: true, result: await crawlNotionUrl(input, deps) });
  } catch (error) {
    reply({ ok: false, error: error instanceof Error ? error.message : String(error) });
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().catch((error) => reply({ ok: false, error: error instanceof Error ? error.message : String(error) }));
}
