/**
 * Discord Ed25519 署名検証 (旧 machina/signature-verify の Discord 分を discord-hook へ移設)。
 */

import assert from "node:assert/strict";
import crypto from "node:crypto";

import { verifyDiscordSignature } from "../../src/discord-hook/signature-verify.js";

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
