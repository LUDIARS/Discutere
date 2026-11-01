import assert from "node:assert/strict";
import { performance } from "node:perf_hooks";
import { test } from "node:test";
import { stripHtml } from "./strip-html.js";
import { extractSpecTextFromBytes, fetchSpecText, readSpecTextFromFile } from "./spec-source.js";

test("HTML text preserves script/style removal, entities, tags and whitespace", () => {
  const fixtures: [string, string][] = [
    ["", ""],
    [" \t<p>Hello <b>world</b></p>\r\n<p>next</p> ", "Hello world next"],
    ['a<ScRiPt type="text/javascript">if (x < 1) hidden()</sCrIpT>b<STYLE>p > a { color:red }</style>c', "a b c"],
    ["&amp; &lt;b&gt; &quot;x&quot; &apos;y&apos; &nbsp; &#65; &#x42;", '& <b> "x" \'y\' A B'],
    ["<scripture>visible</scripture><stylesheet>also</stylesheet>", "visible also"],
    ["before<script>unclosed", "before unclosed"],
    ["before<style>unclosed", "before unclosed"],
    ["before<a ", "before<a"],
    ["<>x<>", "<>x<>"],
    ["a<script>one</script>b<script>two</script>c<style>three</style>d", "a b c d"],
    ["日本語 İ Σ &AMP;", "日本語 İ Σ &"],
    ["<script>inside<style>hidden</style></script><p>visible</p>", "visible"],
    ["<style>inside<script>hidden</script></style><p>visible</p>", "visible"],
  ];
  for (const [html, expected] of fixtures) assert.equal(stripHtml(html), expected);
});

test("URL, local file and upload HTML imports share the text extractor", async () => {
  const bytes = Buffer.from("<p>本文 &amp; 続き</p><SCRIPT>hidden</SCRIPT><style>hidden</style>");
  const expected = "本文 & 続き";
  assert.equal(await extractSpecTextFromBytes(bytes, "spec.html"), expected);
  assert.equal(await fetchSpecText("https://example.invalid/spec.html", {
    fetchImpl: (async () => new Response(bytes, { headers: { "content-type": "text/html" } })) as typeof fetch,
  }), expected);
  assert.equal(await readSpecTextFromFile("spec.htm", {
    fs: {
      existsSync: () => true,
      statSync: () => ({ size: bytes.length }),
      readFileSync: () => bytes,
    } as unknown as NonNullable<Parameters<typeof readSpecTextFromFile>[1]>["fs"],
  }), expected);
});

for (const token of ["<script ", "<style ", "<a "]) {
  test(`256 KiB of unclosed ${token} completes in under one second`, () => {
    const html = token.repeat(Math.ceil(256 * 1024 / token.length));
    const start = performance.now();
    const result = stripHtml(html);
    const elapsed = performance.now() - start;
    assert.equal(result, html.trim());
    assert.ok(elapsed < 1000, `HTML extraction took ${elapsed.toFixed(1)} ms`);
  });
}

test("complete opening tags without closing blocks and tag-name lookalikes stay linear", () => {
  for (const token of ["<script>", "<style>", "<scripture>", "<stylesheet>"]) {
    const html = token.repeat(Math.ceil(256 * 1024 / token.length));
    const start = performance.now();
    assert.equal(stripHtml(html), "");
    assert.ok(performance.now() - start < 1000, `Slow repeated ${token}`);
  }
});
