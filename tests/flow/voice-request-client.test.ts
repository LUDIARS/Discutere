import assert from "node:assert/strict";
import { createVoiceRequestClient, VoiceRequestFailure } from "../../src/flow/user-voices/request-client.js";

let tokens = 0;
const token = async () => { tokens++; return { ok: true as const, token: "test-token" }; };
const down = createVoiceRequestClient(() => "http://localhost:8892", token, async () => { throw new Error("offline"); });
await assert.rejects(down.submit({ requestId: "di:1", theme: "theme" }), e => e instanceof VoiceRequestFailure && e.kind === "down" && !e.deliveryUnknown);
assert.equal(tokens, 0, "Vo 停止時に受付や認証成功を推定しない");

const calls: RequestInit[] = [];
const client = createVoiceRequestClient(() => "http://localhost:8892", token, async (_url, init) => {
  calls.push(init!);
  return init?.method === "POST" ? Response.json({ data: { request: { requestId: "di:1", status: "accepted" } } }) : Response.json({ ok: true });
});
assert.deepEqual(await client.submit({ requestId: "di:1", theme: "theme" }), { accepted: true, requestId: "di:1" });
assert.equal(calls[1].redirect, "manual");
assert.deepEqual(JSON.parse(calls[1].body as string), { requestId: "di:1", theme: "theme" });

for (const status of [401, 403, 503]) {
  const rejected = createVoiceRequestClient(() => "http://localhost:8892", token, async (_url, init) => init?.method === "POST" ? new Response(null, { status }) : Response.json({ ok: true }));
  await assert.rejects(rejected.submit({ requestId: "di:1", theme: "theme" }), e => e instanceof VoiceRequestFailure && e.kind === (status === 503 ? "down" : "auth"));
}
const ambiguous = createVoiceRequestClient(() => "http://localhost:8892", token, async (_url, init) => {
  if (init?.method === "POST") throw new Error("connection lost after send");
  return Response.json({ ok: true });
});
await assert.rejects(ambiguous.submit({ requestId: "di:1", theme: "theme" }), e => e instanceof VoiceRequestFailure && e.deliveryUnknown);
