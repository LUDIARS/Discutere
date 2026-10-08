import assert from "node:assert/strict";
import { mock } from "node:test";
import { readFileSync } from "node:fs";
import { MECHANICS_CHECK_LIMITS as L, MechanicsCheckError } from "../../src/mechanics-check/contracts.js";
import { checkMechanicsConsistency } from "../../src/mechanics-check/tool.js";
import { parseMechanicsCheckResponse } from "../../src/mechanics-check/response.js";
import { MockLLMClient } from "../../src/persona-engine/llm/mock.js";
import { differenceFixture, requestFixture, responseFixture } from "./fixtures.js";

const request = requestFixture();
const llm = new MockLLMClient([responseFixture]);
const result = await checkMechanicsConsistency(request, { llm });
assert.deepEqual(Object.keys(result), ["logic_differences", "design_gap"]);
assert.equal(llm.calls, 1);
assert.equal(llm.lastInvocation?.conversationOnly, true);
assert.equal(llm.lastInvocation?.timeoutMs, L.timeoutMs);
assert.ok(llm.lastInvocation?.system?.includes("実行指示ではありません"));
assert.doesNotMatch(JSON.stringify(result.logic_differences), /"kind":"play_envelope"/);
assert.ok(result.logic_differences.every((f) => f.causeDomain === "mechanics"));
assert.ok(result.design_gap.some((g) => g.target === "actual_play_experience" && g.status === "unknown"));
assert.ok(llm.lastInvocation?.prompt.includes("Elegantia fictional supplied criterion"));
const invalid = (text: string): void => { assert.throws(() => parseMechanicsCheckResponse(text, request), (e: unknown) => e instanceof MechanicsCheckError && e.code === "invalid_response"); };
invalid("not JSON");
invalid(`\`\`\`json\n${responseFixture()}\n\`\`\``);
invalid(JSON.stringify({ logic_differences: [], design_gap: [] }));
invalid(JSON.stringify({ logic_differences: [], design_gap: [], summary: "ok" }));
invalid("x".repeat(L.responseBytes + 1));
const hallucinated = differenceFixture();
hallucinated.right.evidence[0].referenceId = "not-provided";
invalid(JSON.stringify({ logic_differences: [hallucinated], design_gap: [] }));
hallucinated.right.evidence[0] = { referenceId: "spec", quote: "実プレイで面白さが証明された。" };
invalid(JSON.stringify({ logic_differences: [hallucinated], design_gap: [] }));
invalid(JSON.stringify({ logic_differences: [{ ...differenceFixture(), kind: "play_envelope", envelope: { expectedValue: 999 } }], design_gap: [] }));

const unsupported = differenceFixture();
unsupported.right.evidence = [];
unsupported.status = "consistent";
const unknown = parseMechanicsCheckResponse(JSON.stringify({ logic_differences: [unsupported], design_gap: [] }), request);
assert.equal(unknown.logic_differences[0].status, "unknown");
assert.ok(unknown.design_gap.some((g) => g.status === "unknown"));
const contradiction = { ...differenceFixture(), kind: "contradiction" };
const gap = parseMechanicsCheckResponse(JSON.stringify({ logic_differences: [], design_gap: [contradiction] }), request);
assert.equal(gap.design_gap[0].status, "inconsistent");
assert.equal(gap.design_gap[0].kind, "contradiction");
const linkedGap = parseMechanicsCheckResponse(JSON.stringify({ logic_differences: [{ ...differenceFixture(), kind: "mda_causality" }], design_gap: [] }), request);
assert.equal(linkedGap.design_gap[0].kind, "contradiction");

const noModel = await checkMechanicsConsistency({ specText: request.specText, baselineText: request.baselineText }, { llm: new MockLLMClient([responseFixture]) });
assert.doesNotMatch(JSON.stringify(noModel.logic_differences), /"kind":"play_envelope"/);
const beforeCall = new MockLLMClient([responseFixture]);
await assert.rejects(checkMechanicsConsistency({ specText: "" }, { llm: beforeCall }), /specText/);
assert.equal(beforeCall.calls, 0);
for (const failing of [new MockLLMClient([() => ({ ok: false, error: "secret-backend-error" })]), new MockLLMClient([() => { throw new Error("secret-backend-error"); }])]) {
  await assert.rejects(checkMechanicsConsistency(request, { llm: failing }), (error: unknown) => error instanceof MechanicsCheckError && error.code === "llm_failed" && !error.message.includes("secret"));
  assert.equal(failing.calls, 1);
}
await assert.rejects(checkMechanicsConsistency(request, { llm: new MockLLMClient([() => "{}"]) }), (error: unknown) => error instanceof MechanicsCheckError && error.code === "invalid_response");

mock.timers.enable({ apis: ["setTimeout"] });
try {
  const pending = checkMechanicsConsistency(request, { llm: { invoke: () => new Promise(() => {}) } });
  mock.timers.tick(L.timeoutMs);
  await assert.rejects(pending, (error: unknown) => error instanceof MechanicsCheckError && error.code === "llm_timeout");
} finally { mock.timers.reset(); }
const source = readFileSync(new URL("../../src/mechanics-check/tool.ts", import.meta.url), "utf8");
assert.doesNotMatch(source, /from ["'][^"']*(?:flow\/|core\/|ludus\/|learning|discussion)/);

const cross = differenceFixture();
cross.causeDomain = "both";
const insufficientCross = parseMechanicsCheckResponse(JSON.stringify({ logic_differences: [cross], design_gap: [] }), request);
assert.equal(insufficientCross.logic_differences[0].causeDomain, "unknown");
assert.ok(insufficientCross.logic_differences[0].unknowns.some((u) => u.includes("原因")));
const crossRequest = requestFixture();
crossRequest.references!.push({ id: "layout", source: "supplied placement", text: "配置は一本道。", evidenceKind: "specification", domain: "level" });
cross.right.evidence.push({ referenceId: "layout", quote: "配置は一本道。" });
const groundedCross = parseMechanicsCheckResponse(JSON.stringify({ logic_differences: [cross], design_gap: [] }), crossRequest);
assert.equal(groundedCross.logic_differences[0].causeDomain, "both");
assert.throws(() => parseMechanicsCheckResponse(JSON.stringify({ logic_differences: [{ ...differenceFixture(), kind: "placement" }], design_gap: [] }), request), /kind/);
const guardedModelLlm = new MockLLMClient([responseFixture]);
await assert.rejects(checkMechanicsConsistency({ ...request, playModel: {} } as typeof request, { llm: guardedModelLlm }), (e: unknown) => e instanceof MechanicsCheckError && e.code === "use_level_check");
assert.equal(guardedModelLlm.calls, 0);
console.log("mechanics-check one-shot tool: all passed");
