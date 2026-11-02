// src/persona-bridge/{authorization-assertion,utterance-export}.ts の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-A-persona_bridge.gen.ts
// 鍵はテスト専用に固定の種から作る (本物の鍵ではない)。
import fs from "node:fs";
import path from "node:path";
import { createPrivateKey, createPublicKey, sign } from "node:crypto";
import { verifyPersonaBridgeAssertion, PERSONA_BRIDGE_AUDIENCE } from "../../../src/persona-bridge/authorization-assertion.js";
import { decodeUtteranceCursor, encodeUtteranceCursor, nextUtteranceCursor } from "../../../src/persona-bridge/utterance-export.js";

const seed = Buffer.alloc(32, 7);
const pkcs8 = Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), seed]);
const priv = createPrivateKey({ key: pkcs8, format: "der", type: "pkcs8" });
const publicKey = createPublicKey(priv).export({ format: "der", type: "spki" }).toString("base64url");
const other = createPublicKey(createPrivateKey({ key: Buffer.concat([Buffer.from("302e020100300506032b657004220420", "hex"), Buffer.alloc(32, 9)]), format: "der", type: "pkcs8" }))
  .export({ format: "der", type: "spki" }).toString("base64url");
const NOW_MS = Date.parse("2026-07-28T00:00:00.000Z");
const nowS = Math.floor(NOW_MS / 1000);

function signed(payload: unknown, raw?: string): string {
  const p = raw ?? Buffer.from(JSON.stringify(payload), "utf8").toString("base64url");
  return `${p}.${sign(null, Buffer.from(p, "utf8"), priv).toString("base64url")}`;
}
const good = (over: Record<string, unknown> = {}) => ({ authorId: "123456789", aud: PERSONA_BRIDGE_AUDIENCE, exp: nowS + 120, jti: "assertion-jti-0001", ...over });
const cases: Array<{ name: string; compact: string; publicKey: string; authorId: string }> = [
  { name: "ok", compact: signed(good()), publicKey, authorId: "123456789" },
  { name: "ok-max-ttl", compact: signed(good({ exp: nowS + 300 })), publicKey, authorId: "123456789" },
  { name: "ttl-too-long", compact: signed(good({ exp: nowS + 301 })), publicKey, authorId: "123456789" },
  { name: "expired", compact: signed(good({ exp: nowS })), publicKey, authorId: "123456789" },
  { name: "author-mismatch", compact: signed(good()), publicKey, authorId: "987654321" },
  { name: "author-invalid", compact: signed(good({ authorId: "1234" })), publicKey, authorId: "1234" },
  { name: "audience", compact: signed(good({ aud: "another" })), publicKey, authorId: "123456789" },
  { name: "exp-float", compact: signed(good({ exp: nowS + 1.5 })), publicKey, authorId: "123456789" },
  { name: "exp-string", compact: signed(good({ exp: String(nowS + 10) })), publicKey, authorId: "123456789" },
  { name: "jti-short", compact: signed(good({ jti: "short" })), publicKey, authorId: "123456789" },
  { name: "jti-bad-char", compact: signed(good({ jti: "assertion jti 0001" })), publicKey, authorId: "123456789" },
  { name: "payload-array", compact: signed([1, 2]), publicKey, authorId: "123456789" },
  { name: "payload-not-json", compact: signed(null, Buffer.from("not json").toString("base64url")), publicKey, authorId: "123456789" },
  { name: "payload-bad-utf8", compact: signed(null, Buffer.from([0x7b, 0xff, 0x7d]).toString("base64url")), publicKey, authorId: "123456789" },
  { name: "payload-2048", compact: signed(good({ pad: "x".repeat(2048 - JSON.stringify(good({ pad: "" })).length) })), publicKey, authorId: "123456789" },
  { name: "payload-2049", compact: signed(good({ pad: "x".repeat(2049 - JSON.stringify(good({ pad: "" })).length) })), publicKey, authorId: "123456789" },
  { name: "oversized", compact: `${"A".repeat(3000)}.${"A".repeat(86)}`, publicKey, authorId: "123456789" },
  { name: "wrong-key", compact: signed(good()), publicKey: other, authorId: "123456789" },
  { name: "tampered", compact: signed(good()).replace(/^./, (c) => (c === "e" ? "f" : "e")), publicKey, authorId: "123456789" },
  { name: "three-parts", compact: signed(good()) + ".x", publicKey, authorId: "123456789" },
  { name: "empty-part", compact: "." + "A".repeat(86), publicKey, authorId: "123456789" },
  { name: "sig-short", compact: Buffer.from("{}").toString("base64url") + "." + "A".repeat(85), publicKey, authorId: "123456789" },
  { name: "sig-bad-char", compact: Buffer.from("{}").toString("base64url") + "." + "A".repeat(85) + "+", publicKey, authorId: "123456789" },
  { name: "no-key", compact: signed(good()), publicKey: "  ", authorId: "123456789" },
  { name: "bad-key", compact: signed(good()), publicKey: "AAAA", authorId: "123456789" },
  { name: "bad-key-chars", compact: signed(good()), publicKey: publicKey + "=", authorId: "123456789" },
];
const assertions = cases.map((c) => {
  try { return { ...c, nowMs: NOW_MS, ok: true, output: verifyPersonaBridgeAssertion({ ...c, nowMs: NOW_MS }) }; }
  catch (e) { return { ...c, nowMs: NOW_MS, ok: false, errorClass: (e as Error).constructor.name, error: (e as Error).message }; }
});

const cursorInputs = [
  encodeUtteranceCursor({ postedAt: 2000, id: "page-2" }),
  encodeUtteranceCursor({ postedAt: 0, id: "日本語-id" }),
  Buffer.from(JSON.stringify([1.0, "a"])).toString("base64url"),
  Buffer.from(JSON.stringify([1.5, "a"])).toString("base64url"),
  Buffer.from(JSON.stringify([-1, "a"])).toString("base64url"),
  Buffer.from(JSON.stringify([2 ** 53, "a"])).toString("base64url"),
  Buffer.from(JSON.stringify([1, ""])).toString("base64url"),
  Buffer.from(JSON.stringify([1, "x".repeat(256)])).toString("base64url"),
  Buffer.from(JSON.stringify([1, "x".repeat(257)])).toString("base64url"),
  Buffer.from(JSON.stringify([1, "a", 3])).toString("base64url"),
  Buffer.from(JSON.stringify({ a: 1 })).toString("base64url"),
  Buffer.from("[1,\"a\"").toString("base64url"),
  "", "A".repeat(513), "abc+def", "abc=",
];
const cursors = cursorInputs.map((input) => {
  try { return { input, ok: true, output: decodeUtteranceCursor(input) }; }
  catch (e) { return { input, ok: false, error: (e as Error).message }; }
});
const nexts = [
  [], [{ id: "a", text: "t", createdAt: "1970-01-01T00:00:01.000Z" }],
  [{ id: "a", text: "t", createdAt: "x" }, { id: "b", text: "t", createdAt: "2026-07-28T00:00:00.123Z" }],
  [{ id: "a", text: "t", createdAt: "not a date" }],
].map((input) => {
  try { return { input, ok: true, output: nextUtteranceCursor(input) }; }
  catch (e) { return { input, ok: false, error: (e as Error).message }; }
});
const encodes = [{ postedAt: 1000, id: "human" }, { postedAt: 1764288000000, id: "id/with+chars" }].map((c) => ({ input: c, output: encodeUtteranceCursor(c) }));

const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "persona_bridge");
fs.writeFileSync(path.join(dir, "persona_bridge.json"), JSON.stringify({ assertions, cursors, nexts, encodes }, null, 2) + "\n");
console.log(`wrote ${assertions.length} assertion + ${cursors.length} cursor cases`);
