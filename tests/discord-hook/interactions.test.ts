/**
 * Discord Interactions parser + verifier テスト (PR-B).
 */

import assert from "node:assert/strict";
import crypto from "node:crypto";

import {
  channelMessageResponse,
  getInteractionSpeakerId,
  INTERACTION_TYPE,
  interactionToCommand,
  isPing,
  pongResponse,
  verifyAndParseInteraction,
} from "../../src/discord-hook/interactions.js";

// ─── verifier ──────────────────────────────────

{
  const { publicKey, privateKey } = crypto.generateKeyPairSync("ed25519");
  const spki = publicKey.export({ format: "der", type: "spki" }) as Buffer;
  const publicKeyHex = spki.subarray(spki.length - 32).toString("hex");

  const timestamp = String(Math.floor(Date.now() / 1000));
  const interactionBody = {
    id: "i1",
    application_id: "app1",
    type: INTERACTION_TYPE.APPLICATION_COMMAND,
    data: {
      id: "c1",
      name: "propose",
      options: [{ name: "statement", type: 3, value: "選択肢を絞る" }],
    },
    guild_id: "g1",
    channel_id: "ch1",
    member: { user: { id: "u1", username: "alice" } },
    token: "tok",
    version: 1,
  };
  const rawBody = JSON.stringify(interactionBody);
  const sig = crypto
    .sign(null, Buffer.from(timestamp + rawBody), privateKey)
    .toString("hex");

  // 正常: parse + interaction 返却
  const ok = verifyAndParseInteraction({
    rawBody,
    signature: sig,
    timestamp,
    publicKey: publicKeyHex,
  });
  assert.ok(ok.ok);
  if (ok.ok) {
    assert.equal(ok.interaction.type, INTERACTION_TYPE.APPLICATION_COMMAND);
    assert.equal(ok.interaction.data?.name, "propose");
  }
  console.log("ok verifyAndParseInteraction valid");

  // 不正署名 → 401
  const bad = verifyAndParseInteraction({
    rawBody,
    signature: "0".repeat(128),
    timestamp,
    publicKey: publicKeyHex,
  });
  assert.equal(bad.ok, false);
  if (!bad.ok) assert.equal(bad.status, 401);
  console.log("ok verifyAndParseInteraction invalid signature");

  // 不正 JSON → 400 (署名は通る前提で別 body を渡す → tampered 扱いで 401)
  // 実装は verify → JSON.parse の順なので、 JSON 不正は verify が通った後に来る。
  // ここでは verify を通るように rawBody を tamper せず、 別 path で確認。
  const badJsonRaw = "this is not json";
  const badJsonSig = crypto
    .sign(null, Buffer.from(timestamp + badJsonRaw), privateKey)
    .toString("hex");
  const bj = verifyAndParseInteraction({
    rawBody: badJsonRaw,
    signature: badJsonSig,
    timestamp,
    publicKey: publicKeyHex,
  });
  assert.equal(bj.ok, false);
  if (!bj.ok) assert.equal(bj.status, 400);
  console.log("ok verifyAndParseInteraction invalid json");
}

// ─── interactionToCommand ───────────────────

{
  const i1 = {
    id: "i",
    application_id: "a",
    type: INTERACTION_TYPE.APPLICATION_COMMAND,
    data: {
      name: "propose",
      options: [
        { name: "statement", type: 3, value: "選択肢を絞る" },
        { name: "extra", type: 3, value: "詳細" },
      ],
    },
    token: "t",
    version: 1,
  };
  assert.equal(interactionToCommand(i1), "/propose 選択肢を絞る 詳細");
  console.log("ok command with args");

  const i2 = {
    id: "i",
    application_id: "a",
    type: INTERACTION_TYPE.APPLICATION_COMMAND,
    data: { name: "list" },
    token: "t",
    version: 1,
  };
  assert.equal(interactionToCommand(i2), "/list");
  console.log("ok command without args");

  const ping = {
    id: "i",
    application_id: "a",
    type: INTERACTION_TYPE.PING,
    token: "t",
    version: 1,
  };
  assert.equal(interactionToCommand(ping), null);
  assert.equal(isPing(ping), true);
  console.log("ok ping detected, command null");
}

// ─── speaker resolver ────────────────────────

{
  const withMember = {
    id: "i",
    application_id: "a",
    type: INTERACTION_TYPE.APPLICATION_COMMAND,
    member: { user: { id: "u1", username: "alice" } },
    token: "t",
    version: 1,
  };
  assert.equal(getInteractionSpeakerId(withMember), "u1");

  const withUser = {
    id: "i",
    application_id: "a",
    type: INTERACTION_TYPE.APPLICATION_COMMAND,
    user: { id: "u2", username: "bob" },
    token: "t",
    version: 1,
  };
  assert.equal(getInteractionSpeakerId(withUser), "u2");

  const noUser = {
    id: "i",
    application_id: "a",
    type: INTERACTION_TYPE.APPLICATION_COMMAND,
    token: "t",
    version: 1,
  };
  assert.equal(getInteractionSpeakerId(noUser), null);
  console.log("ok speaker resolver");
}

// ─── response helpers ────────────────────

{
  assert.deepEqual(pongResponse(), { type: 1 });
  assert.deepEqual(channelMessageResponse("hi"), {
    type: 4,
    data: { content: "hi" },
  });
  assert.deepEqual(channelMessageResponse("err", { ephemeral: true }), {
    type: 4,
    data: { content: "err", flags: 64 },
  });
  console.log("ok response helpers");
}

console.log("interactions.test.ts: all passed");
