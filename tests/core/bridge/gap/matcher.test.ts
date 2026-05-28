import assert from "node:assert/strict";
import { shouldDetectGap } from "../../../../src/core/bridge/gap/matcher.js";

assert.equal(shouldDetectGap({ intended: "joy", observed: [] }).detect, true);
assert.equal(shouldDetectGap({ intended: "joy", observed: ["joy", "anger", "joy"] }).type, "negative");
assert.equal(shouldDetectGap({ intended: "joy", observed: ["joy", "calm", "joy"] }).type, "mixed");
assert.equal(shouldDetectGap({ intended: "joy", observed: ["joy", "joy", "joy"] }).detect, false);
console.log("gap matcher passed");
