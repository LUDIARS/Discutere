import assert from "node:assert/strict";
import { test } from "node:test";
import { readFileSync } from "node:fs";
import { getYoutubeApiKey, getYoutubeApiKeyStatus } from "../../src/secrets/youtube-env.js";

test("YouTube credentials come only from injected env, including missing and blank cases", async () => {
  assert.equal(await getYoutubeApiKey({}), null);
  assert.equal(await getYoutubeApiKey({ DISCUTERE_YOUTUBE_API_KEY: "  " }), null);
  assert.equal(await getYoutubeApiKey({ DISCUTERE_YOUTUBE_API_KEY: " test-key " }), "test-key");
  assert.equal(await getYoutubeApiKey({ INFISICAL_CLIENT_SECRET: "unused" }), null);
});

test("status contains presence without exposing the secret", () => {
  assert.deepEqual(getYoutubeApiKeyStatus({}), { present: false });
  assert.deepEqual(getYoutubeApiKeyStatus({ DISCUTERE_YOUTUBE_API_KEY: "  " }), { present: false });
  assert.deepEqual(getYoutubeApiKeyStatus({ DISCUTERE_YOUTUBE_API_KEY: "test-key" }), { present: true });
});

test("catalog startup scripts use the injected environment without bootstrap", () => {
  const pkg = JSON.parse(readFileSync(new URL("../../package.json", import.meta.url), "utf8"));
  assert.equal(pkg.scripts.dev, "tsx watch src/index.ts");
  assert.equal(pkg.scripts["dev:server"], "tsx watch src/index.ts");
  assert.ok(!Object.keys(pkg.scripts).some((key) => key.startsWith("env:")));
  assert.ok(!JSON.stringify(pkg).match(/dotenv-cli|env-cli|--env-file/));
});
