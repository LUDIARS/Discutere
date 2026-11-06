// spec-source.ts のテキスト系・HTML・URL 解析の Node 実装の入出力を記録する (PDF/DOCX/ZIP はサイドカーなので対象外)。
// 実行: npx tsx native/tests/golden/w1-E-spec_source.gen.ts → native/tests/fixtures/flow/w1e_spec_source.json
import fs from "node:fs";
import path from "node:path";
import { buildGithubSpecSource, extractSpecTextFromBytes, fetchSpecText, isHttpUrl, isTextual, parseGithubSpecUrl } from "../../../src/flow/spec-source.js";

const urls = [
  "git@github.com:owner/repo.git", " GIT@GitHub.com:o/r ", "git@github.com:o/r.git.git", "git@github.com:o/.git", "git@github.com:o/a b",
  "https://github.com/owner/repo", "https://github.com/owner/repo.git/blob/main/docs/spec.md", "https://github.com/o/r/raw/v1/a/b/c.pdf",
  "https://github.com/o/r/tree/dev", "https://github.com/o/r/tree/dev/sub/dir", "https://github.com/o/r?ref=x&path=y/z.md",
  "https://GitHub.com/o/r/blob/main", "https://raw.githubusercontent.com/o/r/main/spec.md", "https://raw.githubusercontent.com/o/r/main",
  "https://api.github.com/repos/o/r/contents/docs/a.md?ref=feat%2Fx", "https://api.github.com/repos/o/r", "https://api.github.com/repos/o/r/contents",
  "https://github.com/o", "https://gitlab.com/o/r", "not a url", "",
];
const textual = ["", "abc", "a\u0000b", "\x01\x02\x03abcdefghijklmnopqrstuvwxyz", "\x01abcdefghij", "日本語\t\r\n"];
const htmls = [
  "<html><head><title>T</title><style>p{color:red}</style><script>var a = '<b>';</script></head><body><h1>仕様 &amp; 設計</h1><p>本文&nbsp;です &lt;tag&gt; &#x3042;&#12354; &quot;q&quot; &apos;a&apos; &#39;</p></body></html>",
  "<SCRIPT type=x>alert(1)</SCRIPT>after<scripts>x</scripts> <style>never closed",
  "a < b > c <> d <",
  "  \n\t ",
  "﻿<p>BOM</p>",
];
const texts = ["# spec テキスト", "﻿BOM 付き", "bad \xff utf8"];

const enc = (s: string) => new TextEncoder().encode(s);
const extract: unknown[] = [];
for (const h of htmls) extract.push({ name: "a.html", bytes: Buffer.from(h, "utf8").toString("base64"), result: await extractSpecTextFromBytes(enc(h), "a.html") });
for (const t of texts) extract.push({ name: "a.md", bytes: Buffer.from(t, "latin1").toString("base64"), result: await extractSpecTextFromBytes(new Uint8Array(Buffer.from(t, "latin1")), "a.md") });
// 不正な UTF-8 の並び (TextDecoder の置換の単位を確かめる)
const badUtf8 = [Buffer.from([0x61, 0xe3, 0x81, 0x62]), Buffer.from([0xf0, 0x9f, 0x98]), Buffer.from([0xed, 0xa0, 0x80, 0x41]), Buffer.from([0xc0, 0xaf, 0xe0, 0x80, 0x41]), Buffer.from([0xf4, 0x90, 0x80, 0x80])];
for (const b of badUtf8) extract.push({ name: "b.txt", bytes: b.toString("base64"), result: await extractSpecTextFromBytes(new Uint8Array(b), "b.txt") });
const errors: unknown[] = [];
for (const [bytes, name] of [[enc("x"), "a.exe"], [enc("x"), "noext"], [new Uint8Array([0, 1]), "a.md"], [new Uint8Array(0), "a.txt"]] as const) {
  try { await extractSpecTextFromBytes(bytes, name); errors.push({ name, error: null }); }
  catch (e) { errors.push({ name, bytes: Buffer.from(bytes).toString("base64"), error: (e as Error).message }); }
}
const fetches: unknown[] = [];
const fakeFetch = (bytes: Uint8Array, contentType: string, status = 200) => (async () => ({
  ok: status >= 200 && status < 300, status,
  arrayBuffer: async () => bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength),
  headers: { get: (k: string) => (k === "content-type" ? contentType : null) },
})) as unknown as typeof fetch;
for (const [u, body, ct, status] of [
  ["https://x/spec.md", "# 仕様", "", 200], ["https://x/page", "<p>a&amp;b</p>", "text/html; charset=utf-8", 200], ["https://x/p.HTM?q=1", "<b>x</b>", "", 200],
  ["https://x/y", "nope", "", 404], ["https://x/a.exe", "x", "", 200], ["https://x/bin", "\u0000\u0001", "", 200], ["https://x/dir/", "plain", "text/plain", 200],
] as const) {
  try { fetches.push({ u, body, ct, status, result: await fetchSpecText(u, { fetchImpl: fakeFetch(enc(body), ct, status) }) }); }
  catch (e) { fetches.push({ u, body, ct, status, error: (e as Error).message }); }
}
const out = {
  github: urls.map((u) => ({ u, r: parseGithubSpecUrl(u) ?? null })),
  build: [buildGithubSpecSource("https://github.com/o/r", " docs/x.md ", " v2 ") ?? null, buildGithubSpecSource("https://github.com/o/r/blob/m/a.md", "  ", undefined) ?? null, buildGithubSpecSource("nope") ?? null],
  textual: textual.map((t) => ({ b: Buffer.from(t, "latin1").toString("base64"), r: isTextual(new Uint8Array(Buffer.from(t, "latin1"))) })),
  isHttp: ["https://a", " HTTP://b", "ftp://c", "httpx://", "http:/x"].map((s) => ({ s, r: isHttpUrl(s) })),
  extract,
  errors,
  fetches,
};
fs.writeFileSync(path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "flow", "w1e_spec_source.json"), JSON.stringify(out, null, 2) + "\n");
console.log("wrote w1e_spec_source.json");
