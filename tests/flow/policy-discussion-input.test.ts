import assert from "node:assert/strict";

import {
  applyPolicyItems,
  parsePolicyDiscussionInput,
  parsePolicyItems,
} from "../../src/flow/policy-discussion-input.js";

assert.deepEqual(parsePolicyItems("- 継続率を上げる\n2. 初回導線を短くする\n- 継続率を上げる"), [
  "継続率を上げる",
  "初回導線を短くする",
]);
assert.equal(
  applyPolicyItems("対象は新規プレイヤー", ["導線を短くする"]),
  "対象は新規プレイヤー\n\n検討する施策リスト:\n1. 導線を短くする"
);
assert.throws(
  () => parsePolicyDiscussionInput({ discussionInputKind: "specification" }),
  /仕様書/
);
assert.throws(
  () => parsePolicyDiscussionInput({ discussionInputKind: "policy-list", policyItems: "" }),
  /施策/
);
assert.deepEqual(
  parsePolicyDiscussionInput({
    discussionInputKind: "specification",
    specUrl: "spec/feature.md",
  }),
  { kind: "specification", policyItems: [] }
);

console.log("  [ok] policy discussion input: specification/policy-list validation");
