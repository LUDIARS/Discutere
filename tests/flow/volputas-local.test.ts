import assert from "node:assert/strict";

import {
  fetchLocalVolputasPersona,
  formatVolputasPersonaContext,
  resolveVolputasBaseUrl,
  toPersonaDiscussionSnapshot,
} from "../../src/flow/volputas-local.js";

const payload = {
  ok: true,
  data: {
    analysis: {
      schemaVersion: 1,
      modelVersion: "evidence-persona-v1",
      analyzedAt: "2026-07-27T00:00:00.000Z",
      axes: {
        exploration: { label: "探索志向", score: 80, evidenceWeight: 2 },
        mastery: { label: "習熟志向", score: 60, evidenceWeight: 1 },
      },
      leadingAxes: [
        { id: "exploration", label: "探索志向", score: 80, evidenceWeight: 2 },
      ],
      evidence: { surveys: 1, gameplay: 2, voices: 3, emotionCurves: 4 },
      note: "推定です。",
    },
    evidenceCount: 10,
    stale: false,
  },
};

const calls: Array<{ url: string; method: string }> = [];
const fetchFn = (async (input: string | URL | Request, init?: RequestInit) => {
  calls.push({ url: String(input), method: init?.method ?? "GET" });
  return new Response(JSON.stringify(payload), {
    status: 200,
    headers: { "content-type": "application/json" },
  });
}) as typeof fetch;

const status = await fetchLocalVolputasPersona({
  baseUrl: "http://127.0.0.1:9999",
  refresh: true,
  fetchFn,
});
assert.equal(status.evidenceCount, 10);
assert.deepEqual(calls.map((call) => call.method), ["POST", "GET"]);
const snapshot = toPersonaDiscussionSnapshot(status);
assert.equal(snapshot.schema, "ludiars.persona-discussion-snapshot");
assert.equal(snapshot.facets[0].dimensions[0].scale.max, 100);
assert.equal(snapshot.evidenceSummary.total, 10);
assert.match(formatVolputasPersonaContext(status), /探索志向.*80\/100/);
assert.match(formatVolputasPersonaContext(status), /アンケート 1/);
assert.throws(() => resolveVolputasBaseUrl({ VOLPUTAS_URL: "https://example.com" }), /loopback/);
assert.throws(() => resolveVolputasBaseUrl({}), /Excubitor topology/);

console.log("  [ok] local Volputas persona: loopback fetch + discussion context");
