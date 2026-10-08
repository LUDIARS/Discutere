import assert from "node:assert/strict";
import { LEVEL_CHECK_LIMITS as L, LevelCheckError } from "../../src/level-check/contracts.js";
import { validateLevelCheckRequest } from "../../src/level-check/request.js";
import { requestFixture } from "./fixtures.js";

const invalid = (value: unknown): void => { assert.throws(() => validateLevelCheckRequest(value), (error: unknown) => error instanceof LevelCheckError && error.code === "invalid_input"); };
const validated = validateLevelCheckRequest(requestFixture());
assert.equal(validated.references?.[0].evidenceKind, "specification");
assert.equal(validated.playModel?.scenarios[0].outcomes.length, 3);
invalid({ levelText: " " });
invalid({ levelText: "x".repeat(L.documentChars + 1) });
invalid({ levelText: "x", fetch: "https://example.com" });
invalid({ levelText: "x", references: [{ id: "level", source: "x", text: "x", evidenceKind: "observation" }] });
invalid({ levelText: "x", references: Array.from({ length: L.references + 1 }, (_, i) => ({ id: `r${i}`, source: "x", text: "x", evidenceKind: "specification" })) });
invalid({ levelText: "x".repeat(20_000), baselineText: "x".repeat(20_000), references: Array.from({ length: 4 }, (_, i) => ({ id: `r${i}`, source: "x", text: "x".repeat(8_000), evidenceKind: "specification" })) });

for (const mutate of [
  (r: ReturnType<typeof requestFixture>) => { r.references!.push({ ...r.references![0] }); },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.events.push({ ...r.playModel!.events[0] }); },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.events[0].evidenceRefs = ["not-supplied"]; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.events[0].evidenceRefs = ["level", "level"]; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].eventIds = ["missing-event"]; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].eventIds = ["bonus", "bonus"]; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].unit = "seconds"; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].predictedBaseline!.unit = "seconds"; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].value = Infinity; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].probability = NaN; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].probability = -0.1; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].probability = 1.1; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].probability = 0.1; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[0].probability = 0.6; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios.push(structuredClone(r.playModel!.scenarios[0])); },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[1].id = "low"; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].outcomes[1].eventIds = []; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].assumptions.trialCount = 1.5; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].assumptions.timeLimitSeconds = 0; },
  (r: ReturnType<typeof requestFixture>) => { r.playModel!.scenarios[0].assumptions.other = Array(9).fill("condition"); },
]) {
  const request = requestFixture();
  mutate(request);
  invalid(request);
}
const partial = requestFixture();
delete partial.playModel!.scenarios[0].outcomes[1].probability;
assert.doesNotThrow(() => validateLevelCheckRequest(partial));
console.log("level-check request validation: all passed");
