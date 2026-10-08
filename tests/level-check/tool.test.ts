import assert from "node:assert/strict";
import { mock } from "node:test";
import { readFileSync } from "node:fs";
import { LEVEL_CHECK_LIMITS as L, LevelCheckError } from "../../src/level-check/contracts.js";
import { checkLevelConsistency } from "../../src/level-check/tool.js";
import { parseLevelCheckResponse } from "../../src/level-check/response.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";
import { differenceFixture, requestFixture, responseFixture } from "./fixtures.js";

const request = requestFixture();
const llm = new MockLLMClient([responseFixture]);
const result = await checkLevelConsistency(request, { llm });
assert.deepEqual(Object.keys(result), ["logic_differences", "design_gap"]);
assert.equal(llm.calls, 1);
assert.equal(llm.lastInvocation?.conversationOnly, true);
assert.equal(llm.lastInvocation?.timeoutMs, L.timeoutMs);
assert.ok(llm.lastInvocation?.system?.includes("実行指示ではありません"));
assert.ok(result.logic_differences.some((f) => f.kind === "play_envelope" && f.envelope.expectedValue === 60 && f.causeDomain === "unknown"));
assert.ok(result.design_gap.some((g) => g.target === "actual_level_experience" && g.status === "unknown"));
assert.ok(llm.lastInvocation?.prompt.includes("Elegantia fictional supplied criterion"));
const invalid = (text: string): void => { assert.throws(() => parseLevelCheckResponse(text, request), (e: unknown) => e instanceof LevelCheckError && e.code === "invalid_response"); };
invalid("not JSON");
invalid(`\`\`\`json\n${responseFixture()}\n\`\`\``);
invalid(JSON.stringify({ logic_differences: [], design_gap: [] }));
invalid(JSON.stringify({ logic_differences: [], design_gap: [], summary: "ok" }));
invalid("x".repeat(L.responseBytes + 1));
const hallucinated = differenceFixture();
hallucinated.right.evidence[0].referenceId = "not-provided";
invalid(JSON.stringify({ logic_differences: [hallucinated], design_gap: [] }));
hallucinated.right.evidence[0] = { referenceId: "level", quote: "実プレイで面白さが証明された。" };
invalid(JSON.stringify({ logic_differences: [hallucinated], design_gap: [] }));
invalid(JSON.stringify({ logic_differences: [{ ...differenceFixture(), kind: "play_envelope", envelope: { expectedValue: 999 } }], design_gap: [] }));

const unsupported = differenceFixture();
unsupported.right.evidence = [];
unsupported.status = "consistent";
const unknown = parseLevelCheckResponse(JSON.stringify({ logic_differences: [unsupported], design_gap: [] }), request);
assert.equal(unknown.logic_differences[0].status, "unknown");
assert.ok(unknown.design_gap.some((g) => g.status === "unknown"));
const contradiction = { ...differenceFixture(), kind: "contradiction" };
const gap = parseLevelCheckResponse(JSON.stringify({ logic_differences: [], design_gap: [contradiction] }), request);
assert.equal(gap.design_gap[0].status, "inconsistent");
assert.equal(gap.design_gap[0].kind, "contradiction");
const linkedGap = parseLevelCheckResponse(JSON.stringify({ logic_differences: [{ ...differenceFixture(), kind: "scenario_condition" }], design_gap: [] }), request);
assert.equal(linkedGap.design_gap[0].kind, "contradiction");

const noModel = await checkLevelConsistency({ levelText: request.levelText, baselineText: request.baselineText }, { llm: new MockLLMClient([responseFixture]) });
assert.ok(noModel.logic_differences.every((f) => f.kind !== "play_envelope"));
const beforeCall = new MockLLMClient([responseFixture]);
await assert.rejects(checkLevelConsistency({ levelText: "" }, { llm: beforeCall }), /levelText/);
assert.equal(beforeCall.calls, 0);
for (const failing of [new MockLLMClient([() => ({ ok: false, error: "secret-backend-error" })]), new MockLLMClient([() => { throw new Error("secret-backend-error"); }])]) {
  await assert.rejects(checkLevelConsistency(request, { llm: failing }), (error: unknown) => error instanceof LevelCheckError && error.code === "llm_failed" && !error.message.includes("secret"));
  assert.equal(failing.calls, 1);
}
await assert.rejects(checkLevelConsistency(request, { llm: new MockLLMClient([() => "{}"]) }), (error: unknown) => error instanceof LevelCheckError && error.code === "invalid_response");

mock.timers.enable({ apis: ["setTimeout"] });
try {
  const pending = checkLevelConsistency(request, { llm: { invoke: () => new Promise(() => {}) } });
  mock.timers.tick(L.timeoutMs);
  await assert.rejects(pending, (error: unknown) => error instanceof LevelCheckError && error.code === "llm_timeout");
} finally { mock.timers.reset(); }
const source = readFileSync(new URL("../../src/level-check/tool.ts", import.meta.url), "utf8");
assert.doesNotMatch(source, /from ["'][^"']*(?:flow\/|core\/|ludus\/|learning|discussion)/);

const attribution = differenceFixture();
attribution.causeDomain = "both";
const unknownCause = parseLevelCheckResponse(JSON.stringify({ logic_differences: [attribution], design_gap: [] }), request);
assert.equal(unknownCause.logic_differences[0].causeDomain, "unknown");
const withRules = requestFixture();
withRules.references!.push({ id: "rules", source: "supplied rules", text: "成功で得点を得る。", evidenceKind: "specification", domain: "mechanics" });
attribution.left.evidence.push({ referenceId: "rules", quote: "成功で得点を得る。" });
assert.equal(parseLevelCheckResponse(JSON.stringify({ logic_differences: [attribution], design_gap: [] }), withRules).logic_differences[0].causeDomain, "both");
console.log("level-check one-shot tool: all passed");
