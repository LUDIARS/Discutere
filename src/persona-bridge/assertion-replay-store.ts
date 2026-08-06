import type Database from "better-sqlite3";

import { getFlowDb } from "../flow/db/connection.js";
import type { VerifiedPersonaBridgeAssertion } from "./authorization-assertion.js";

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export type AssertionReplayConsumer = (
  assertion: VerifiedPersonaBridgeAssertion,
  nowMs: number
) => boolean;

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export function consumePersonaBridgeAssertion(
  db: Database.Database,
  assertion: VerifiedPersonaBridgeAssertion,
  nowMs: number
): boolean {
  const nowSeconds = Math.floor(nowMs / 1_000);
  return db.transaction(() => {
    db.prepare("DELETE FROM persona_bridge_assertion_nonce WHERE expires_at <= ?").run(nowSeconds);
    return db.prepare(
      "INSERT OR IGNORE INTO persona_bridge_assertion_nonce (jti, expires_at) VALUES (?, ?)"
    ).run(assertion.jti, assertion.expiresAt).changes === 1;
  })();
}

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export const consumeDefaultPersonaBridgeAssertion: AssertionReplayConsumer =
  (assertion, nowMs) => consumePersonaBridgeAssertion(getFlowDb(), assertion, nowMs);
