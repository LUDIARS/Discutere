/**
 * Webhook 署名検証 (Slack HMAC-SHA256 / Discord Ed25519)。
 *
 * Slack: https://api.slack.com/authentication/verifying-requests-from-slack
 *   - base = "v0:" + timestamp + ":" + rawBody
 *   - signature = "v0=" + hex(hmac_sha256(signingSecret, base))
 *   - timestamp は |now - ts| <= 300s でないと replay 扱い
 *
 * Discord: https://discord.com/developers/docs/interactions/receiving-and-responding#security-and-authorization
 *   - message = timestamp + rawBody
 *   - sig (Ed25519 hex 64 bytes) を publicKey (hex 32 bytes) で verify
 *
 * 失敗時は false 返し。 例外は内部に閉じる (= 不正 input でも 500 にしない)。
 */

import crypto from "node:crypto";

export interface VerifySlackArgs {
  rawBody: string;
  signature: string; // "v0=..."
  timestamp: string; // unix sec string
  signingSecret: string;
  /** default 300 sec */
  toleranceSec?: number;
  /** test 用 — Date.now() の差替 */
  now?: () => number;
}

export function verifySlackSignature(args: VerifySlackArgs): boolean {
  const tolerance = args.toleranceSec ?? 300;
  if (!args.signature || !args.timestamp || !args.signingSecret) return false;
  const ts = Number(args.timestamp);
  if (!Number.isFinite(ts)) return false;
  const now = Math.floor((args.now?.() ?? Date.now()) / 1000);
  if (Math.abs(now - ts) > tolerance) return false;

  const base = `v0:${args.timestamp}:${args.rawBody}`;
  const computed =
    "v0=" + crypto.createHmac("sha256", args.signingSecret).update(base).digest("hex");
  return timingSafeEqualStr(computed, args.signature);
}

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

function timingSafeEqualStr(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  try {
    return crypto.timingSafeEqual(Buffer.from(a), Buffer.from(b));
  } catch {
    return false;
  }
}
