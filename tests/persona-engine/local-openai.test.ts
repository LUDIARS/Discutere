/**
 * LocalOpenAiClient (OpenAI 互換ローカル LLM backend) のテスト。
 *
 * fetch を差し替えて、 chat/completions のリクエスト整形・レスポンス解釈・
 * usage マップ・エラー処理を検証する (実エンドポイント不要)。
 */
import assert from "node:assert/strict";

import { LocalOpenAiClient } from "../../src/persona-engine/llm/local-openai.js";

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json" },
  });
}

// ── 正常系: choices[0].message.content を返し usage をマップする ──
{
  let captured: { url: string; body: any } | null = null;
  const fetchImpl = (async (url: string | URL | Request, init?: RequestInit) => {
    captured = { url: String(url), body: JSON.parse(String(init?.body)) };
    return jsonResponse({
      choices: [{ message: { content: "  やあ、 描野です。  " } }],
      usage: { prompt_tokens: 12, completion_tokens: 8 },
    });
  }) as unknown as typeof fetch;

  const client = new LocalOpenAiClient({
    baseUrl: "http://localhost:11434/v1/",
    defaultModel: "gemma3:12b",
    fetchImpl,
  });
  const res = await client.invoke({ prompt: "自己紹介して", system: "あなたはスケッチ屋。" });

  assert.equal(res.ok, true, "正常レスポンスは ok");
  if (res.ok) {
    assert.equal(res.text, "やあ、 描野です。", "content を trim して返す");
    assert.deepEqual(res.usage, { input_tokens: 12, output_tokens: 8 }, "usage を TokenUsage にマップ");
  }
  assert.ok(captured, "fetch が呼ばれる");
  assert.equal(captured!.url, "http://localhost:11434/v1/chat/completions", "末尾スラッシュ正規化 + /chat/completions");
  assert.equal(captured!.body.model, "gemma3:12b", "model を送る");
  assert.equal(captured!.body.messages[0].role, "system", "system を先頭メッセージに");
  assert.equal(captured!.body.messages[1].content, "自己紹介して", "user prompt を送る");
  console.log("ok local-openai 正常系 (整形 + usage マップ)");
}

// ── HTTP エラーは ok:false ──
{
  const fetchImpl = (async () => jsonResponse({ error: "model not found" }, 404)) as unknown as typeof fetch;
  const client = new LocalOpenAiClient({ baseUrl: "http://x/v1", defaultModel: "m", fetchImpl });
  const res = await client.invoke({ prompt: "x" });
  assert.equal(res.ok, false, "404 は ok:false");
  if (!res.ok) assert.ok(res.error.includes("404"), "status をエラーに含む");
  console.log("ok local-openai HTTP エラー");
}

// ── baseUrl / model 欠落は呼ぶ前に ok:false ──
{
  const noUrl = await new LocalOpenAiClient({ baseUrl: "", defaultModel: "m" }).invoke({ prompt: "x" });
  assert.equal(noUrl.ok, false, "baseUrl 無しは ok:false");
  const noModel = await new LocalOpenAiClient({ baseUrl: "http://x/v1", defaultModel: "" }).invoke({ prompt: "x" });
  assert.equal(noModel.ok, false, "model 無しは ok:false");
  console.log("ok local-openai 設定欠落ガード");
}

console.log("local-openai tests: all passed");
