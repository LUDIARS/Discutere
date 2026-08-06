import { createPublicKey, verify as verifySignature } from "node:crypto";

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export const PERSONA_BRIDGE_AUDIENCE = "discutere-persona-bridge";
export const MAX_ASSERTION_TTL_SECONDS = 5 * 60;

const SNOWFLAKE_PATTERN = /^[0-9]{5,24}$/;
const JTI_PATTERN = /^[A-Za-z0-9_-]{16,128}$/;
const BASE64URL_PATTERN = /^[A-Za-z0-9_-]+$/;
const MAX_PAYLOAD_BYTES = 2_048;
const MAX_PAYLOAD_SEGMENT_LENGTH = Math.ceil(MAX_PAYLOAD_BYTES * 4 / 3);
const ED25519_SIGNATURE_SEGMENT_LENGTH = 86;

export interface VerifiedPersonaBridgeAssertion {
  authorId: string;
  expiresAt: number;
  jti: string;
}

export class PersonaBridgeAssertionConfigurationError extends Error {}
export class PersonaBridgeAssertionVerificationError extends Error {}

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
function decodeBase64Url(value: string, label: string): Buffer {
  if (!BASE64URL_PATTERN.test(value)) {
    throw new PersonaBridgeAssertionVerificationError(`${label} is not base64url`);
  }
  return Buffer.from(value, "base64url");
}

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
function assertionPublicKey(encoded: string) {
  try {
    if (!BASE64URL_PATTERN.test(encoded)) throw new Error("not base64url");
    const key = createPublicKey({
      key: Buffer.from(encoded, "base64url"),
      format: "der",
      type: "spki",
    });
    if (key.asymmetricKeyType !== "ed25519") throw new Error("not Ed25519");
    return key;
  } catch {
    throw new PersonaBridgeAssertionConfigurationError(
      "DISCUTERE_PERSONA_BRIDGE_ASSERTION_PUBLIC_KEY must be an Ed25519 SPKI DER key encoded as base64url"
    );
  }
}

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
function assertionPayload(segment: string): Record<string, unknown> {
  const bytes = decodeBase64Url(segment, "assertion payload");
  if (bytes.length === 0 || bytes.length > MAX_PAYLOAD_BYTES) {
    throw new PersonaBridgeAssertionVerificationError("assertion payload size is invalid");
  }
  try {
    const parsed = JSON.parse(bytes.toString("utf8")) as unknown;
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
      throw new Error("not an object");
    }
    return parsed as Record<string, unknown>;
  } catch {
    throw new PersonaBridgeAssertionVerificationError("assertion payload is invalid JSON");
  }
}

// @implements SPEC-PERSONA-BRIDGE-UTTERANCE-EXPORT
export function verifyPersonaBridgeAssertion({
  compact,
  publicKey,
  authorId,
  nowMs = Date.now(),
}: {
  compact: string;
  publicKey: string;
  authorId: string;
  nowMs?: number;
}): VerifiedPersonaBridgeAssertion {
  if (!publicKey.trim()) {
    throw new PersonaBridgeAssertionConfigurationError(
      "DISCUTERE_PERSONA_BRIDGE_ASSERTION_PUBLIC_KEY is required"
    );
  }
  const segments = compact.split(".");
  if (segments.length !== 2 || !segments[0] || !segments[1]) {
    throw new PersonaBridgeAssertionVerificationError("assertion format is invalid");
  }
  const payloadSegment = segments[0]!;
  const signatureSegment = segments[1]!;
  if (
    payloadSegment.length > MAX_PAYLOAD_SEGMENT_LENGTH
    || signatureSegment.length !== ED25519_SIGNATURE_SEGMENT_LENGTH
  ) {
    throw new PersonaBridgeAssertionVerificationError("assertion size is invalid");
  }
  const signature = decodeBase64Url(signatureSegment, "assertion signature");
  if (signature.length !== 64 || !verifySignature(
    null,
    Buffer.from(payloadSegment, "utf8"),
    assertionPublicKey(publicKey),
    signature
  )) {
    throw new PersonaBridgeAssertionVerificationError("assertion signature is invalid");
  }

  const payload = assertionPayload(payloadSegment);
  const assertedAuthorId = payload.authorId;
  const audience = payload.aud;
  const expiresAt = payload.exp;
  const jti = payload.jti;
  if (typeof assertedAuthorId !== "string" || !SNOWFLAKE_PATTERN.test(assertedAuthorId)) {
    throw new PersonaBridgeAssertionVerificationError("assertion authorId is invalid");
  }
  if (assertedAuthorId !== authorId) {
    throw new PersonaBridgeAssertionVerificationError("assertion authorId does not match the query");
  }
  if (audience !== PERSONA_BRIDGE_AUDIENCE) {
    throw new PersonaBridgeAssertionVerificationError("assertion audience is invalid");
  }
  if (typeof expiresAt !== "number" || !Number.isInteger(expiresAt)) {
    throw new PersonaBridgeAssertionVerificationError("assertion exp is invalid");
  }
  if (typeof jti !== "string" || !JTI_PATTERN.test(jti)) {
    throw new PersonaBridgeAssertionVerificationError("assertion jti is invalid");
  }
  const nowSeconds = Math.floor(nowMs / 1_000);
  if (expiresAt <= nowSeconds || expiresAt > nowSeconds + MAX_ASSERTION_TTL_SECONDS) {
    throw new PersonaBridgeAssertionVerificationError("assertion is expired or not short-lived");
  }
  return { authorId: assertedAuthorId, expiresAt, jti };
}
