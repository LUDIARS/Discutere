/**
 * ③ 仕様書ソース解決 (spec-source.ts) テスト。
 * - URL fetch (text 系のみ・サイズ上限・非200/バイナリ拒否)
 * - ローカルパス読み (fs 注入・サイズ上限・存在チェック)
 * - resolveSpecText の http(s)→fetch / それ以外→パス読み (allowLocalPath ゲート)
 * - isTextual のバイナリ判定
 * fetch / fs は注入してネットワーク・ディスク非依存で検証する。
 */

import assert from "node:assert/strict";
import {
  fetchSpecText,
  readSpecTextFromFile,
  resolveSpecText,
  isHttpUrl,
  isTextual,
} from "../../src/flow/spec-source.js";

/** text → fetch 風 Response を返す fakeFetch を作る。 */
function fakeFetch(opts: { ok?: boolean; status?: number; bytes: Uint8Array }): typeof fetch {
  return (async () =>
    ({
      ok: opts.ok ?? true,
      status: opts.status ?? 200,
      arrayBuffer: async () => opts.bytes.buffer.slice(opts.bytes.byteOffset, opts.bytes.byteOffset + opts.bytes.byteLength),
    }) as unknown as Response) as unknown as typeof fetch;
}

const enc = (s: string) => new TextEncoder().encode(s);

// ── isTextual ────────────────────────────────────────────────────────────────
assert.equal(isTextual(enc("# Spec\nメカニクス: ジャンプ")), true, "テキストは textual");
assert.equal(isTextual(new Uint8Array([0x50, 0x4b, 0x03, 0x00, 0x01])), false, "NUL 含みはバイナリ");
assert.equal(isTextual(new Uint8Array(0)), false, "空はテキスト扱いしない");
console.log("  [ok] spec-source: isTextual バイナリ判定");

// ── fetchSpecText ────────────────────────────────────────────────────────────
const text = await fetchSpecText("https://example.com/spec.md", {
  fetchImpl: fakeFetch({ bytes: enc("# 仕様\n連鎖でスコア") }),
});
assert.match(text, /連鎖でスコア/, "URL から本文を取得");

await assert.rejects(
  () => fetchSpecText("https://x/y", { fetchImpl: fakeFetch({ ok: false, status: 404, bytes: enc("nope") }) }),
  /HTTP 404/,
  "非200 は throw"
);

await assert.rejects(
  () => fetchSpecText("https://x/y", { maxBytes: 5, fetchImpl: fakeFetch({ bytes: enc("0123456789") }) }),
  /大きすぎます/,
  "サイズ上限超過は throw"
);

await assert.rejects(
  () => fetchSpecText("https://x/bin", { fetchImpl: fakeFetch({ bytes: new Uint8Array([0x00, 0x01, 0x02]) }) }),
  /テキストとして読めません/,
  "バイナリは throw"
);
console.log("  [ok] spec-source: fetchSpecText (取得/非200/上限/バイナリ)");

// ── readSpecTextFromFile (fs 注入) ──────────────────────────────────────────
const fakeFs = {
  existsSync: (p: string) => p === "/spec.md",
  statSync: (() => ({ size: 20 })) as unknown as typeof import("node:fs").statSync,
  readFileSync: ((p: string) => Buffer.from("# ローカル仕様")) as unknown as typeof import("node:fs").readFileSync,
};
const local = readSpecTextFromFile("/spec.md", { fs: fakeFs });
assert.match(local, /ローカル仕様/, "ローカルファイルから本文");
assert.throws(() => readSpecTextFromFile("/missing.md", { fs: fakeFs }), /見つかりません/, "不在は throw");
console.log("  [ok] spec-source: readSpecTextFromFile (読み/不在)");

// ── resolveSpecText ルーティング + allowLocalPath ゲート ─────────────────────
assert.equal(isHttpUrl("https://a.b/c"), true);
assert.equal(isHttpUrl("spec/foo.md"), false);

const viaUrl = await resolveSpecText("https://x/spec.md", {
  fetchImpl: fakeFetch({ bytes: enc("URL経由仕様") }),
});
assert.match(viaUrl, /URL経由仕様/, "http(s) は fetch 経路");

await assert.rejects(
  () => resolveSpecText("spec/foo.md", { allowLocalPath: false }),
  /許可されていません/,
  "allowLocalPath=false でローカルパスは拒否 (Discord 経路)"
);

const viaPath = await resolveSpecText("/spec.md", { allowLocalPath: true, fs: fakeFs });
assert.match(viaPath, /ローカル仕様/, "allowLocalPath=true でパス読み (Web 経路)");
console.log("  [ok] spec-source: resolveSpecText ルーティング + allowLocalPath ゲート");

console.log("spec-source (③) tests: all passed");
