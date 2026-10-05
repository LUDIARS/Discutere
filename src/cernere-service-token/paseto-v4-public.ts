import { createPublicKey, verify as verifySignature, type KeyObject } from "node:crypto";

/**
 * PASETO v4.public (Ed25519) の署名検証だけを行う最小実装。
 *
 * Cernere service token の受け側検証に使う。 paseto パッケージを依存に足さず、
 * 仕様 (PAE + Ed25519) を node:crypto で直接たどる。 claims の意味 (kind / aud /
 * exp / scope) はここでは見ない (service-token-verify.ts の責務)。
 */
const HEADER = "v4.public.";
const SIGNATURE_BYTES = 64;
// Ed25519 SubjectPublicKeyInfo の DER prefix。 raw 32 byte を後ろに連結する。
const ED25519_SPKI_PREFIX = Buffer.from("302a300506032b6570032100", "hex");

export class PasetoFormatError extends Error {}

export function isPasetoV4Public(token: string): boolean {
  return token.startsWith(HEADER);
}

export function ed25519PublicKeyFromRaw(raw: Buffer): KeyObject {
  if (raw.length !== 32) throw new PasetoFormatError("Ed25519 public key must be 32 bytes");
  return createPublicKey({
    key: Buffer.concat([ED25519_SPKI_PREFIX, raw]),
    format: "der",
    type: "spki",
  });
}

function le64(value: number): Buffer {
  const out = Buffer.alloc(8);
  out.writeBigUInt64LE(BigInt(value) & 0x7fffffffffffffffn);
  return out;
}

/** Pre-Authentication Encoding (PASETO 共通)。 */
export function pae(pieces: Buffer[]): Buffer {
  return Buffer.concat([
    le64(pieces.length),
    ...pieces.flatMap((piece) => [le64(piece.length), piece]),
  ]);
}

function decodeBase64Url(value: string): Buffer {
  if (!/^[A-Za-z0-9_-]*$/.test(value)) throw new PasetoFormatError("invalid base64url segment");
  return Buffer.from(value, "base64url");
}

/**
 * 署名が鍵のどれか 1 つで通れば payload (JSON object) を返す。
 * 形式不正・署名不一致は例外。 implicit assertion は Cernere と同じく空。
 */
export function verifyPasetoV4Public(token: string, keys: readonly KeyObject[]): Record<string, unknown> {
  if (!isPasetoV4Public(token)) throw new PasetoFormatError("not a v4.public token");
  const parts = token.slice(HEADER.length).split(".");
  if (parts.length < 1 || parts.length > 2 || !parts[0]) {
    throw new PasetoFormatError("malformed v4.public token");
  }
  const body = decodeBase64Url(parts[0]);
  if (body.length <= SIGNATURE_BYTES) throw new PasetoFormatError("v4.public token is too short");
  const footer = parts[1] === undefined ? Buffer.alloc(0) : decodeBase64Url(parts[1]);
  const message = body.subarray(0, body.length - SIGNATURE_BYTES);
  const signature = body.subarray(body.length - SIGNATURE_BYTES);
  const signed = pae([Buffer.from(HEADER, "utf8"), message, footer, Buffer.alloc(0)]);
  const valid = keys.some((key) => verifySignature(null, signed, key, signature));
  if (!valid) throw new PasetoFormatError("signature verification failed");
  const payload: unknown = JSON.parse(message.toString("utf8"));
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    throw new PasetoFormatError("payload must be a JSON object");
  }
  return payload as Record<string, unknown>;
}
