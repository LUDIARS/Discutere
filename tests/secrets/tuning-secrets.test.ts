import assert from "node:assert/strict";
import { test } from "node:test";
import { tuningRoutes } from "../../src/api/tuning-routes.js";

test("legacy secret mutation and refresh endpoints are absent", async () => {
  for (const [method, suffix] of [["PUT", ""], ["POST", "/refresh"], ["POST", "/gcloud"]]) {
    const response = await tuningRoutes.request(`/admin/tuning/secrets/youtube-api-key${suffix}`, {
      method,
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ apiKey: "test-key" }),
    });
    assert.equal(response.status, 404);
  }
});

test("tuning UI directs secret changes to Vault without a key input", async () => {
  const response = await tuningRoutes.request("/admin/tuning");
  assert.equal(response.status, 200);
  const html = await response.text();
  assert.match(html, /Excubitor Vault/);
  assert.doesNotMatch(html, /yt_apiKey|saveYoutubeKey|refreshYoutubeKey|importYoutubeKeyFromGcloud/);
});
