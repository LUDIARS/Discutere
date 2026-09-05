/**
 * モデル編成 (flow.roster) — model spec の分解と generateFlowPersonas の割当。
 */
import assert from "node:assert/strict";

const { parseModelSpec, isCodexModel, modelOfSpec, validateModelSpec } = await import("../../src/persona-engine/llm/model-spec.js");
const { generateFlowPersonas, rosterPersonaCount } = await import("../../src/flow/personas.js");
const { ModelRouterLlm } = await import("../../src/flow/model-router.js");
const { CodexCliClient, buildCodexEnvironment } = await import("../../src/persona-engine/llm/codex-cli.js");
const { modelFamily } = await import("../../src/persona-engine/llm/pricing.js");

// ── model spec ──
assert.deepEqual(parseModelSpec("claude-opus-5@xhigh"), { model: "claude-opus-5", effort: "xhigh" });
assert.deepEqual(parseModelSpec("gpt-5.6-sol@mid"), { model: "gpt-5.6-sol", effort: "medium" });
assert.deepEqual(parseModelSpec("claude-fable-5-1"), { model: "claude-fable-5-1" });
assert.deepEqual(parseModelSpec(""), { model: "" });
assert.equal(isCodexModel("gpt-5.6-terra@high"), true);
assert.equal(isCodexModel("claude-sonnet-5@high"), false);
assert.equal(modelOfSpec("claude-sonnet-5@high"), "claude-sonnet-5");
assert.equal(modelFamily("claude-opus-5@xhigh"), "opus", "pricing は effort を無視する");
assert.equal(validateModelSpec(parseModelSpec("gpt-5.6-sol@high"), "codex"), null);
assert.match(
  validateModelSpec(parseModelSpec("gpt-5.6-sol@max"), "codex") ?? "",
  /未対応/,
  "provider が受けない effort は起動前に拒否する"
);
assert.match(
  validateModelSpec(parseModelSpec("gpt-5.6-sol&whoami@high"), "codex") ?? "",
  /使用できない文字/,
  "shell メタ文字を含む model は拒否する"
);

// ── roster 割当 ──
const roster = {
  facilitator: "claude-fable-5-1",
  discussants: ["claude-opus-5@xhigh", "gpt-5.6-sol@xhigh", "claude-sonnet-5@high"],
};
assert.equal(rosterPersonaCount(4, roster), 4);
assert.equal(rosterPersonaCount(9, undefined), 9);
assert.equal(rosterPersonaCount(9, { discussants: [] }), 9);

const rng = () => 0.25;
const cast = generateFlowPersonas({ count: rosterPersonaCount(4, roster), defaultModel: "x", isLocal: false, rng, roster });
assert.equal(cast.length, 4);
assert.equal(cast[0].role, "facilitator");
assert.equal(cast[0].model, "claude-fable-5-1");
assert.deepEqual(
  cast.slice(1).map((p) => p.model),
  roster.discussants,
  "議論者は discussants を順に 1 回ずつ使う"
);

// 人数が discussants より多いときは巡回
const more = generateFlowPersonas({ count: 6, defaultModel: "x", isLocal: false, rng, roster });
assert.deepEqual(
  more.slice(1).map((p) => p.model),
  [...roster.discussants, ...roster.discussants.slice(0, 2)]
);

// roster 無しは従来どおり defaultModel
const plain = generateFlowPersonas({ count: 3, defaultModel: "claude-haiku-4-5", isLocal: false, rng });
assert.ok(plain.every((p) => p.model === "claude-haiku-4-5"));

// ── router ──
const calls: string[] = [];
const mk = (tag: string) => ({
  invoke: async (a: { model?: string }) => {
    calls.push(`${tag}:${a.model}`);
    return { ok: true as const, text: tag };
  },
});
const router = new ModelRouterLlm(mk("claude"), mk("codex"));
assert.equal((await router.invoke({ prompt: "p", model: "gpt-5.6-sol@xhigh" })).ok, true);
assert.equal((await router.invoke({ prompt: "p", model: "claude-opus-5@xhigh" })).ok, true);
assert.deepEqual(calls, ["codex:gpt-5.6-sol@xhigh", "claude:claude-opus-5@xhigh"]);
const noCodex = new ModelRouterLlm(mk("claude"), null);
const r = await noCodex.invoke({ prompt: "p", model: "gpt-5.6-sol" });
assert.equal(r.ok, false, "codex 経路が無ければ Claude に代弁させずエラー");
const invalidCodex = await new CodexCliClient({ cliPath: "must-not-spawn" }).invoke({
  prompt: "p",
  model: "gpt-5.6-sol&whoami@high",
});
assert.equal(invalidCodex.ok, false, "不正な model は subprocess を起動せず拒否する");
const childEnv = buildCodexEnvironment({
  PATH: "bin",
  USERPROFILE: "profile",
  ANTHROPIC_API_KEY: "redacted-test-value",
  CONCORDIA_HOOK: "redacted-test-value",
  LICTOR_PORT: "12345",
});
assert.deepEqual(childEnv, { PATH: "bin", USERPROFILE: "profile" }, "資格情報・内部 endpoint・session 変数を継承しない");

console.log("model-roster: ok");
