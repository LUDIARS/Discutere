/**
 * ペーパー入力の Notion リンク展開 (notion-link.ts) テスト。
 * - 「深さ:N」指示の解釈 (クランプ)
 * - token あり: Canalis crawlPage で子ページを既定深さ 2 まで / 指示深さで辿る
 * - token なし: 公開ページ取得へ回す
 * - 取得失敗は warn して "" (議論を止めない)
 * Notion API / 公開ページ取得は注入して、ネットワーク非依存で検証する。
 */

import assert from "node:assert/strict";
import type { NotionApi, NotionBlock, NotionPage, Paged } from "@ludiars/canalis";
import { expandNotionLinks, parseNotionDepth, MAX_NOTION_DEPTH } from "../../src/flow/notion-link.js";

const ROOT_HEX = "0123456789abcdef0123456789abcdef";
const ROOT_ID = "01234567-89ab-cdef-0123-456789abcdef";
const URL = `https://www.notion.so/ws/Spec-${ROOT_HEX}`;

function paged<T>(results: T[]): Paged<T> {
  return { results, next_cursor: null, has_more: false };
}

/** root → c1 → c2 → c3 の子ページ連鎖を返す fake。 */
function chainApi(): NotionApi {
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
    getBlockChildren: async (id: string) => paged(tree[id] ?? []),
    retrievePage: async (id: string): Promise<NotionPage> => ({
      id,
      url: `https://www.notion.so/${id}`,
      properties: { title: { type: "title", title: [{ plain_text: `T-${id}` }] } },
    }),
  };
}

// ── parseNotionDepth ─────────────────────────────────────────────────────────
assert.equal(parseNotionDepth("深さ:3 で読んで"), 3);
assert.equal(parseNotionDepth("depth=1"), 1);
assert.equal(parseNotionDepth("深さ：99"), MAX_NOTION_DEPTH, "上限にクランプ");
assert.equal(parseNotionDepth("指示なし"), undefined);
console.log("  [ok] notion-link: 深さ指示の解釈");

// ── リンク無し ───────────────────────────────────────────────────────────────
assert.equal(await expandNotionLinks("https://example.com/x だけ", { deps: { api: chainApi() } }), "");
console.log("  [ok] notion-link: Notion リンク無しは空");

// ── token あり: 既定深さ 2 ───────────────────────────────────────────────────
{
  const md = await expandNotionLinks(`仕様: ${URL}`, { deps: { api: chainApi() } });
  assert.match(md, /^# Notion 資料/);
  assert.match(md, /ルート本文/);
  assert.match(md, new RegExp(`## T-${ROOT_ID}`));
  assert.match(md, /### T-c1/);
  assert.match(md, /#### T-c2/);
  assert.doesNotMatch(md, /T-c3/, "既定深さ 2 より深い子ページは取らない");
  console.log("  [ok] notion-link: 既定は深さ 2 まで子ページを辿る");
}

// ── token あり: 本文の深さ指示で上書き ───────────────────────────────────────
{
  const md = await expandNotionLinks(`${URL} 深さ:0`, { deps: { api: chainApi() } });
  assert.doesNotMatch(md, /T-c1/, "深さ:0 は起点ページのみ");
  const deep = await expandNotionLinks(`${URL} 深さ:3`, { deps: { api: chainApi() } });
  assert.match(deep, /T-c3/, "深さ:3 なら c3 まで");
  console.log("  [ok] notion-link: 深さ指示で上書き");
}

// ── token なし: 公開ページ取得 ───────────────────────────────────────────────
{
  const seen: string[] = [];
  const md = await expandNotionLinks(`${URL}。`, {
    deps: {
      api: null,
      fetchPublic: async (url) => {
        seen.push(url);
        return { title: "公開ページ", markdown: "公開本文" };
      },
    },
  });
  assert.deepEqual(seen, [URL], "末尾の句読点を除いた URL で取得");
  assert.match(md, /## 公開ページ/);
  assert.match(md, /公開本文/);
  console.log("  [ok] notion-link: token 無しは公開ページ取得");
}

// ── 取得失敗は warn して空 ───────────────────────────────────────────────────
{
  const warns: string[] = [];
  const md = await expandNotionLinks(URL, {
    deps: {
      api: null,
      fetchPublic: async () => {
        throw new Error("timeout");
      },
    },
    warn: (m) => warns.push(m),
  });
  assert.equal(md, "");
  assert.ok(warns.some((w) => w.includes("timeout")), "失敗理由を warn に出す");
  console.log("  [ok] notion-link: 取得失敗は warn して空");
}
