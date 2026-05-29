import assert from "node:assert/strict";
import { canTransition } from "../../../src/core/hypothesis/state-machine.js";

assert.equal(canTransition("draft", "submit").ok, true);
assert.equal(canTransition("proposed", "validate_theory").ok, true);
assert.equal(canTransition("validated_by_theory", "validate_emotion").ok, true);
// 議論モード向け: proposed から validate_emotion 直接遷移も許可
assert.equal(canTransition("proposed", "validate_emotion").ok, true);
assert.equal(canTransition("validated_by_emotion", "integrate").ok, true);
assert.equal(canTransition("proposed", "integrate").ok, false);
console.log("hypothesis state-machine passed");
