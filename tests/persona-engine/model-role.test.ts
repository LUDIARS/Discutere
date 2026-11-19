import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { AnthropicSdkClient } from "../../src/persona-engine/llm/anthropic.js";
import { isCodexModel, parseModelSpec } from "../../src/persona-engine/llm/model-spec.js";

test("model roles are routed with effort and explicit IDs", () => {
  for (const spec of ["astra", "sol@xhigh", "luna@medium", "codex-mini-latest"]) {
    assert.equal(isCodexModel(spec), true, spec);
  }
  for (const spec of ["opus@xhigh", "sonnet", "haiku", "fable", "claude-test-explicit", undefined]) {
    assert.equal(isCodexModel(spec), false, spec);
  }
  assert.deepEqual(parseModelSpec("sol@mid"), { model: "sol", effort: "medium" });
});

test("Anthropic sends the mapped model, effort and explicit override to the API", async (t) => {
  const dir = mkdtempSync(join(tmpdir(), "di-model-roles-"));
  const mapPath = join(dir, "model-roles.json");
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  t.mock.method(globalThis, "fetch", async (_url: string, init: RequestInit) => {
    requests.push(JSON.parse(init.body as string));
    return Response.json({ content: [{ type: "text", text: "ok" }] });
  });
  const originalPath = process.env.LUDIARS_MODEL_ROLES_FILE;
  const originalHaiku = process.env.LUDIARS_ONESHOT_MODEL_HAIKU;
  t.after(() => {
    if (originalPath === undefined) delete process.env.LUDIARS_MODEL_ROLES_FILE;
    else process.env.LUDIARS_MODEL_ROLES_FILE = originalPath;
    if (originalHaiku === undefined) delete process.env.LUDIARS_ONESHOT_MODEL_HAIKU;
    else process.env.LUDIARS_ONESHOT_MODEL_HAIKU = originalHaiku;
  });
  process.env.LUDIARS_MODEL_ROLES_FILE = mapPath;
  delete process.env.LUDIARS_ONESHOT_MODEL_HAIKU;
  writeFileSync(mapPath, JSON.stringify({ schemaVersion: 1, roles: { claude: { haiku: "claude-test-mapped" } } }));

  const requests: Array<{ model: string; output_config?: { effort: string } }> = [];
  const client = new AnthropicSdkClient({ apiKey: "test-key" });
  assert.equal((await client.invoke({ prompt: "test" })).ok, true);
  process.env.LUDIARS_ONESHOT_MODEL_HAIKU = "claude-test-role-override";
  assert.equal((await client.invoke({ prompt: "test", model: "haiku@high" })).ok, true);
  assert.equal((await client.invoke({ prompt: "test", model: "claude-test-explicit@medium" })).ok, true);
  delete process.env.LUDIARS_ONESHOT_MODEL_HAIKU;
  rmSync(mapPath);
  const missing = await client.invoke({ prompt: "test" });
  assert.equal(missing.ok, false);
  assert.match(missing.error ?? "", /model_role_unresolved:haiku/);
  assert.deepEqual(requests.map(({ model, output_config }) => [model, output_config?.effort]), [
    ["claude-test-mapped", undefined],
    ["claude-test-role-override", "high"],
    ["claude-test-explicit", "medium"],
  ]);
});
