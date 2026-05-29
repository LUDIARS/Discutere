/**
 * JWT_SECRET production guard テスト (PR-C / MISSING #4-5).
 */

import assert from "node:assert/strict";

import {
  assertProductionJwtSecret,
  JwtSecretError,
} from "../../src/auth/jwt-guard.js";

// dev は何があっても通る
assertProductionJwtSecret({ nodeEnv: "development", jwtSecret: "" });
assertProductionJwtSecret({ nodeEnv: "test", jwtSecret: "short" });
assertProductionJwtSecret({
  nodeEnv: "development",
  jwtSecret: "discutere-dev-secret-change-in-production",
});
console.log("ok dev passes regardless");

// production + default secret → throw
assert.throws(
  () =>
    assertProductionJwtSecret({
      nodeEnv: "production",
      jwtSecret: "discutere-dev-secret-change-in-production",
    }),
  JwtSecretError
);
console.log("ok production rejects default dev secret");

// production + 空文字 → throw
assert.throws(
  () => assertProductionJwtSecret({ nodeEnv: "production", jwtSecret: "" }),
  JwtSecretError
);
console.log("ok production rejects empty secret");

// production + short (< 32) → throw
assert.throws(
  () =>
    assertProductionJwtSecret({
      nodeEnv: "production",
      jwtSecret: "short-secret-not-enough",
    }),
  JwtSecretError
);
console.log("ok production rejects short secret");

// production + 強い secret → ok
assertProductionJwtSecret({
  nodeEnv: "production",
  jwtSecret: "this-is-a-strong-32-char-secret-AAA",
});
console.log("ok production accepts strong secret");

console.log("jwt-guard.test.ts: all passed");
