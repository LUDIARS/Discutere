import assert from "node:assert/strict";
import Database from "better-sqlite3";
import { parseConversation } from "../../src/external-discussion/contracts.js";
import { ExternalDiscussionService } from "../../src/external-discussion/service.js";
import { ParticipationStore } from "../../src/external-discussion/store.js";
import { opportunity } from "../../src/external-discussion/participation.js";
import { externalDiscussionRoutes } from "../../src/api/external-discussion-routes.js";

const db = new Database(":memory:");
try {
  let now = 1_000_000;
  let calls = 0;
  const service = new ExternalDiscussionService(new ParticipationStore(db), {
    async invoke(args) { assert.equal(args.conversationOnly, true); calls++; return { ok: true, text: JSON.stringify({ text: "段階的に試すと影響を確認できそうです。" }) }; },
  }, "configured-model", () => now, () => 0);
  const input = parseConversation({ scene: "actio:team:thread", enabled: true,
    messages: [{ id: "1", kind: "human", text: "どう進めよう", at: now - 179_999 }] });
  assert.equal(opportunity(input, now), null);
  now++;
  const first = await service.consider(input);
  assert.equal(first.status, "proposal");
  assert.deepEqual(await service.consider(input), first);
  assert.equal(calls, 1, "replay must not generate another proposal");
  assert.equal((await service.consider({ ...input, enabled: false })).status, "disabled");
  assert.equal(opportunity({ ...input, messages: [...input.messages, { id: "ai", kind: "ai", text: "応答", at: now }] }, now), null);
  const routes = externalDiscussionRoutes(service, "secret");
  assert.equal((await routes.request("/consider", { method: "POST", body: JSON.stringify(input) })).status, 401);
  assert.throws(() => parseConversation({ ...input, messages: [input.messages[0], input.messages[0]] }));
  // One busy-conversation attempt cannot be redrawn on every poll.
  const skip = new ExternalDiscussionService(new ParticipationStore(db), { async invoke() { throw new Error("should not generate"); } }, "model", () => now, () => 0.99);
  const active = { ...input, scene: "actio:another:thread", messages: [0, 1, 2].map(i => ({ id: String(i), kind: "human" as const, text: "検討中", at: now - 1000 + i })) };
  assert.equal((await skip.consider(active)).status, "skipped");
  assert.equal((await skip.consider(active)).status, "skipped");
  assert.equal(calls, 1);
} finally { db.close(); }
