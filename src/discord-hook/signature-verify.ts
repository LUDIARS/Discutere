/**
 * Discord Interactions の Ed25519 署名検証。
 *
 * Discord: https://discord.com/developers/docs/interactions/receiving-and-responding#security-and-authorization
 *   - message = timestamp + rawBody
 *   - sig (Ed25519 hex 64 bytes) を publicKey (hex 32 bytes) で verify
 *
 * 失敗時は false 返し。 例外は内部に閉じる (= 不正 input でも 500 にしない)。
 *
 * 注: HTTP Interactions Endpoint 自体は WS Gateway 再設計 (2026-05-30) で撤去済みだが、
 * interactions.ts のパーサ群は再利用のため保持しており、その署名検証だけここに残す
 * (旧 src/machina/signature-verify.ts の Discord 分。Slack HMAC は machina 撤去で削除)。
 */

import crypto from "node:crypto";

export interface VerifyDiscordArgs {
  rawBody: string;
  signature: string; // hex (128 chars)
  timestamp: string;
  publicKey: string; // hex (64 chars)
}

/**
 * Ed25519 raw 32-byte public key を SPKI DER で包んで Node の crypto.verify に渡す。
 * Discord は raw hex で公開鍵を配るので、 直接 KeyObject は作れない。
 *
 * SPKI prefix (12 bytes): 30 2a 30 05 06 03 2b 65 70 03 21 00
 *   = SEQUENCE(2a) [ SEQUENCE(05) [ OID(03) 1.3.101.112 (Ed25519) ] BIT STRING(21) <unused=00> + 32-byte key ]
 */
const ED25519_SPKI_PREFIX = Buffer.from("302a300506032b6570032100", "hex");

export function verifyDiscordSignature(args: VerifyDiscordArgs): boolean {
  if (!args.signature || !args.timestamp || !args.publicKey) return false;
  try {
    const sig = Buffer.from(args.signature, "hex");
    const pub = Buffer.from(args.publicKey, "hex");
    if (sig.length !== 64 || pub.length !== 32) return false;
    const key = crypto.createPublicKey({
      key: Buffer.concat([ED25519_SPKI_PREFIX, pub]),
      format: "der",
      type: "spki",
    });
    const message = Buffer.from(args.timestamp + args.rawBody);
    return crypto.verify(null, message, key, sig);
  } catch {
    return false;
  }
}
