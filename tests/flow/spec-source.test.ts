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
  extractSpecTextFromBytes,
  isHttpUrl,
  isTextual,
  isPdfBytes,
  isZipBytes,
} from "../../src/flow/spec-source.js";

/** text → fetch 風 Response を返す fakeFetch を作る。 */
function fakeFetch(opts: { ok?: boolean; status?: number; bytes: Uint8Array; contentType?: string }): typeof fetch {
  return (async () =>
    ({
      ok: opts.ok ?? true,
      status: opts.status ?? 200,
      arrayBuffer: async () => opts.bytes.buffer.slice(opts.bytes.byteOffset, opts.bytes.byteOffset + opts.bytes.byteLength),
      headers: { get: (k: string) => k === "content-type" ? (opts.contentType ?? "") : null },
    }) as unknown as Response) as unknown as typeof fetch;
}

/** PDF 抽出を偽装する fakePdfExtractor。 */
const fakePdfExtractor = async (buf: Buffer): Promise<{ text: string }> => {
  void buf;
  return { text: "PDF から抽出したテキスト" };
};
/** DOCX 抽出を偽装する fakeDocxExtractor。 */
const fakeDocxExtractor = async (opts: { buffer: Buffer }): Promise<{ value: string }> => {
  void opts;
  return { value: "DOCX から抽出したテキスト" };
};

const enc = (s: string) => new TextEncoder().encode(s);

// ── isTextual ────────────────────────────────────────────────────────────────
assert.equal(isTextual(enc("# Spec\nメカニクス: ジャンプ")), true, "テキストは textual");
assert.equal(isTextual(new Uint8Array([0x50, 0x4b, 0x03, 0x00, 0x01])), false, "NUL 含みはバイナリ");
assert.equal(isTextual(new Uint8Array(0)), false, "空はテキスト扱いしない");
console.log("  [ok] spec-source: isTextual バイナリ判定");

// ── isPdfBytes / isZipBytes ─────────────────────────────────────────────────
assert.equal(isPdfBytes(new Uint8Array([0x25, 0x50, 0x44, 0x46, 0x2d])), true, "%PDF- はPDF");
assert.equal(isPdfBytes(enc("not pdf")), false, "テキストはPDFでない");
assert.equal(isZipBytes(new Uint8Array([0x50, 0x4b, 0x03, 0x04, 0x00])), true, "PK\\x03\\x04 はZIP/DOCX");
assert.equal(isZipBytes(enc("not zip")), false, "テキストはZIPでない");
console.log("  [ok] spec-source: isPdfBytes / isZipBytes");

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
// PDF URL (Content-Type で判定)
const pdfText = await fetchSpecText("https://x/doc.pdf", {
  fetchImpl: fakeFetch({ bytes: enc("dummy"), contentType: "application/pdf" }),
  pdfExtractor: fakePdfExtractor,
});
assert.match(pdfText, /PDF から抽出/, "PDF URL は pdfExtractor 経路");
// DOCX URL (拡張子で判定)
const docxText = await fetchSpecText("https://x/doc.docx", {
  fetchImpl: fakeFetch({ bytes: enc("dummy") }),
  docxExtractor: fakeDocxExtractor,
});
assert.match(docxText, /DOCX から抽出/, "DOCX URL は docxExtractor 経路");
console.log("  [ok] spec-source: fetchSpecText (取得/非200/上限/バイナリ/PDF/DOCX)");

// ── readSpecTextFromFile (fs 注入) ──────────────────────────────────────────
const fakeFs = {
  existsSync: (p: string) => ["/spec.md", "/doc.pdf", "/doc.docx"].includes(p),
  statSync: (() => ({ size: 20 })) as unknown as typeof import("node:fs").statSync,
  readFileSync: (() => Buffer.from("# ローカル仕様")) as unknown as typeof import("node:fs").readFileSync,
};
const local = await readSpecTextFromFile("/spec.md", { fs: fakeFs });
assert.match(local, /ローカル仕様/, "ローカルファイルから本文");
await assert.rejects(() => readSpecTextFromFile("/missing.md", { fs: fakeFs }), /見つかりません/, "不在は throw");
// PDF ファイル (拡張子で判定)
const localPdf = await readSpecTextFromFile("/doc.pdf", { fs: fakeFs, pdfExtractor: fakePdfExtractor });
assert.match(localPdf, /PDF から抽出/, "PDF ファイルは pdfExtractor 経路");
// DOCX ファイル (拡張子で判定)
const localDocx = await readSpecTextFromFile("/doc.docx", { fs: fakeFs, docxExtractor: fakeDocxExtractor });
assert.match(localDocx, /DOCX から抽出/, "DOCX ファイルは docxExtractor 経路");
console.log("  [ok] spec-source: readSpecTextFromFile (読み/不在/PDF/DOCX)");

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

// ── extractSpecTextFromBytes ────────────────────────────────────────────────
const textBytes = enc("# spec テキスト");
const extractedText = await extractSpecTextFromBytes(textBytes, "spec.md");
assert.match(extractedText, /spec テキスト/, "テキスト系バイトは UTF-8 デコード");
const pdfBytes = new Uint8Array([0x25, 0x50, 0x44, 0x46]); // %PDF
const extractedPdf = await extractSpecTextFromBytes(pdfBytes, "doc.pdf", { pdfExtractor: fakePdfExtractor });
assert.match(extractedPdf, /PDF から抽出/, "PDF マジックバイト → pdfExtractor");
const docxBytes = new Uint8Array([0x50, 0x4b, 0x03, 0x04]); // PK
const extractedDocx = await extractSpecTextFromBytes(docxBytes, "doc.docx", { docxExtractor: fakeDocxExtractor });
assert.match(extractedDocx, /DOCX から抽出/, "DOCX 拡張子 → docxExtractor");
console.log("  [ok] spec-source: extractSpecTextFromBytes (テキスト/PDF/DOCX)");

console.log("spec-source (③) tests: all passed");
