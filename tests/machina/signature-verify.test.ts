/**
 * Slack HMAC + Discord Ed25519 署名検証 (PR-A).
 */

import assert from "node:assert/strict";
import crypto from "node:crypto";

import {
  verifySlackSignature,
  verifyDiscordSignature,
} from "../../src/machina/signature-verify.js";

// ─── Slack HMAC ─────────────────────────────────

{
  const signingSecret = "shh-this-is-secret";
  const timestamp = String(Math.floor(Date.now() / 1000));
  const rawBody = JSON.stringify({ type: "event_callback", event: { type: "message", text: "hello" } });

  const base = `v0:${timestamp}:${rawBody}`;
  const signature = "v0=" + crypto.createHmac("sha256", signingSecret).update(base).digest("hex");

  assert.equal(
    verifySlackSignature({ rawBody, signature, timestamp, signingSecret }),
    true,
    "valid signature should verify"
  );
  console.log("ok slack valid signature");

  // bad signature
  assert.equal(
    verifySlackSignature({
      rawBody,
      signature: "v0=" + "0".repeat(64),
      timestamp,
      signingSecret,
    }),
    false,
    "wrong signature should reject"
  );
  console.log("ok slack wrong signature rejected");

  // stale timestamp (10 min ago) → tolerance 300s でアウト
  const stale = String(Math.floor(Date.now() / 1000) - 600);
  const staleBase = `v0:${stale}:${rawBody}`;
  const staleSig =
    "v0=" + crypto.createHmac("sha256", signingSecret).update(staleBase).digest("hex");
  assert.equal(
    verifySlackSignature({ rawBody, signature: staleSig, timestamp: stale, signingSecret }),
    false,
    "stale timestamp should reject"
  );
  console.log("ok slack stale timestamp rejected");

  // 必須欠落
  assert.equal(verifySlackSignature({ rawBody, signature: "", timestamp, signingSecret }), false);
  assert.equal(verifySlackSignature({ rawBody, signature, timestamp: "", signingSecret }), false);
  assert.equal(verifySlackSignature({ rawBody, signature, timestamp, signingSecret: "" }), false);
  console.log("ok slack missing fields rejected");
}

// ─── Discord Ed25519 ──────────────────────────

{
  // Node の crypto.generateKeyPairSync で Ed25519 鍵生成 → raw 32 byte 抽出
  const { publicKey, privateKey } = crypto.generateKeyPairSync("ed25519");

  // raw public key (32 bytes) を hex で取得
  const spkiDer = publicKey.export({ format: "der", type: "spki" }) as Buffer;
  // SPKI prefix (12 bytes) を剥がす
  const rawPub = spkiDer.subarray(spkiDer.length - 32);
  assert.equal(rawPub.length, 32);
  const publicKeyHex = rawPub.toString("hex");

  const timestamp = String(Math.floor(Date.now() / 1000));
  const rawBody = JSON.stringify({ type: 1, application_id: "x" });
  const message = Buffer.from(timestamp + rawBody);
  const sig = crypto.sign(null, message, privateKey);
  const signature = sig.toString("hex");

  assert.equal(
    verifyDiscordSignature({ rawBody, signature, timestamp, publicKey: publicKeyHex }),
    true,
    "valid Ed25519 signature should verify"
  );
  console.log("ok discord valid signature");

  // body 改ざん → reject
  assert.equal(
    verifyDiscordSignature({
      rawBody: rawBody + "tampered",
      signature,
      timestamp,
      publicKey: publicKeyHex,
    }),
    false,
    "tampered body should reject"
  );
  console.log("ok discord tampered body rejected");

  // 不正 hex 長
  assert.equal(
    verifyDiscordSignature({
      rawBody,
      signature: "deadbeef",
      timestamp,
      publicKey: publicKeyHex,
    }),
    false,
    "wrong-length signature should reject"
  );
  console.log("ok discord wrong-length signature rejected");

  // public key 不一致 → reject
  const { publicKey: otherPub } = crypto.generateKeyPairSync("ed25519");
  const otherSpki = otherPub.export({ format: "der", type: "spki" }) as Buffer;
  const otherRaw = otherSpki.subarray(otherSpki.length - 32).toString("hex");
  assert.equal(
    verifyDiscordSignature({ rawBody, signature, timestamp, publicKey: otherRaw }),
    false,
    "different public key should reject"
  );
  console.log("ok discord wrong public key rejected");

  // 必須欠落
  assert.equal(verifyDiscordSignature({ rawBody, signature: "", timestamp, publicKey: publicKeyHex }), false);
  assert.equal(verifyDiscordSignature({ rawBody, signature, timestamp: "", publicKey: publicKeyHex }), false);
  assert.equal(verifyDiscordSignature({ rawBody, signature, timestamp, publicKey: "" }), false);
  console.log("ok discord missing fields rejected");
}

console.log("signature-verify.test.ts: all passed");
