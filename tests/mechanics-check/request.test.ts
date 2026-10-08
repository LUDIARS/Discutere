import assert from "node:assert/strict";
import { MECHANICS_CHECK_LIMITS as L, MechanicsCheckError } from "../../src/mechanics-check/contracts.js";
import { validateMechanicsCheckRequest } from "../../src/mechanics-check/request.js";
import { requestFixture } from "./fixtures.js";

const invalid = (value: unknown): void => { assert.throws(() => validateMechanicsCheckRequest(value), (error: unknown) => error instanceof MechanicsCheckError && error.code === "invalid_input"); };
const validated = validateMechanicsCheckRequest(requestFixture());
assert.equal(validated.references?.[0].evidenceKind, "specification");
assert.ok(!Object.prototype.hasOwnProperty.call(validated, "playModel"));
invalid({ specText: " " });
invalid({ specText: "x".repeat(L.documentChars + 1) });
invalid({ specText: "x", fetch: "https://example.com" });
invalid({ specText: "x", references: [{ id: "spec", source: "x", text: "x", evidenceKind: "observation" }] });
invalid({ specText: "x", references: Array.from({ length: L.references + 1 }, (_, i) => ({ id: `r${i}`, source: "x", text: "x", evidenceKind: "specification" })) });
invalid({ specText: "x".repeat(20_000), baselineText: "x".repeat(20_000), references: Array.from({ length: 4 }, (_, i) => ({ id: `r${i}`, source: "x", text: "x".repeat(8_000), evidenceKind: "specification" })) });

const invalidModel = new (class { playModel = {}; specText = "spec"; })();
assert.throws(() => validateMechanicsCheckRequest(invalidModel), (e: unknown) => e instanceof MechanicsCheckError && e.code === "use_level_check" && e.message.includes("/level-check"));
assert.throws(() => validateMechanicsCheckRequest({ specText: "spec", playModel: undefined }), (e: unknown) => e instanceof MechanicsCheckError && e.code === "use_level_check");
const duplicates = requestFixture();
duplicates.references!.push({ ...duplicates.references![0] });
invalid(duplicates);
invalid({ specText: "x", references: [{ id: "a", source: "x", text: "x", evidenceKind: "specification", domain: "both" }] });
console.log("mechanics-check request validation: all passed");
