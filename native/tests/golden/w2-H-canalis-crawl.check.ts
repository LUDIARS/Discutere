// native/sidecar/canalis-crawl.ts の取得手順 (crawlNotionUrl) を、本物の Canalis の crawlPage + 偽の Notion API で確かめる。
// tests/flow/notion-link.test.ts の「token あり: 既定深さ 2 / 深さ指示で上書き / token なし」の観点を、サイドカーの中身に対して行う。
// ctest (native/tests/flow/notion_link_tests.cpp) が node --import tsx で起動し、標準出力の JSON を照合する。ネットワークには触れない。
import { crawlPage, isNotionUrl, parseNotionPageId, type NotionApi, type NotionBlock, type NotionPage, type Paged } from "@ludiars/canalis";
import { crawlNotionUrl, parseCrawlInput, type CrawlDeps } from "../../sidecar/canalis-crawl.js";

const ROOT_HEX = "0123456789abcdef0123456789abcdef";
const ROOT_ID = "01234567-89ab-cdef-0123-456789abcdef";
const URL = `https://www.notion.so/ws/Spec-${ROOT_HEX}`;

function paged<T>(results: T[]): Paged<T> {
  return { results, next_cursor: null, has_more: false };
}

function chainApi(empty = false): NotionApi {
  const tree: Record<string, NotionBlock[]> = {
    [ROOT_ID]: [
      { id: "p0", type: "paragraph", paragraph: { rich_text: [{ plain_text: "ルート本文" }] } },
      { id: "c1", type: "child_page", child_page: { title: "C1" } },
    ],
    c1: [{ id: "c2", type: "child_page", child_page: { title: "C2" } }],
    c2: [{ id: "c3", type: "child_page", child_page: { title: "C3" } }],
    c3: [],
  };
  return {
    queryDatabase: async (): Promise<Paged<NotionPage>> => paged([]),
    getBlockChildren: async (id: string) => {
      if (empty) throw new Error("forbidden");
      return paged(tree[id] ?? []);
    },
    retrievePage: async (id: string): Promise<NotionPage> => {
      if (empty) throw new Error("forbidden");
      return { id, url: `https://www.notion.so/${id}`, properties: { title: { type: "title", title: [{ plain_text: `T-${id}` }] } } };
    },
  };
}

const publicCalls: Array<{ url: string; maxDepth: number; maxPages: number }> = [];
function deps(api: NotionApi | null): CrawlDeps {
  return {
    api,
    isNotionUrl,
    parseNotionPageId,
    crawlPage: (a, pageId, opts) => crawlPage(a as NotionApi, pageId, opts) as never,
    crawlPublic: async (url, opts) => {
      publicCalls.push({ url, ...opts });
      return { pages: [{ url, title: "公開ページ", depth: 0, markdown: "公開本文" }], errors: [{ url: "https://acme.notion.site/Gone", message: "404" }], truncated: true };
    },
  };
}

const titles = (r: { pages: Array<{ title: string; depth: number }> }) => r.pages.map((p) => `${p.depth}:${p.title}`);
const out = {
  depth2: titles(await crawlNotionUrl({ url: URL, depth: 2, maxPages: 50 }, deps(chainApi()))),
  depth0: titles(await crawlNotionUrl({ url: URL, depth: 0, maxPages: 50 }, deps(chainApi()))),
  depth3: titles(await crawlNotionUrl({ url: URL, depth: 3, maxPages: 50 }, deps(chainApi()))),
  truncated: (await crawlNotionUrl({ url: URL, depth: 3, maxPages: 2 }, deps(chainApi()))).warnings,
  apiFallback: await crawlNotionUrl({ url: URL, depth: 1, maxPages: 5 }, deps(chainApi(true))),
  noToken: await crawlNotionUrl({ url: URL, depth: 2, maxPages: 7 }, deps(null)),
  publicCalls,
  inputs: [
    { url: URL, depth: 0, maxPages: 1 },
    { url: URL, depth: 5, maxPages: 200 },
    { url: URL, depth: 6, maxPages: 1 },
    { url: URL, depth: 1, maxPages: 0 },
    { url: URL, depth: 1, maxPages: 201 },
    { url: URL, depth: 1.5, maxPages: 1 },
    { url: "https://example.com/x", depth: 1, maxPages: 1 },
    { url: `https://www.notion.so/${"a".repeat(2100)}`, depth: 1, maxPages: 1 },
    [],
    null,
  ].map((i) => parseCrawlInput(i, isNotionUrl) !== null),
};
process.stdout.write(`${JSON.stringify(out)}\n`);
