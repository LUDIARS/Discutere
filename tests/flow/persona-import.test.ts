import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const directory = path.resolve(".tmp/flow-test/persona-import");
fs.mkdirSync(directory, { recursive: true });
process.env.DATABASE_PATH = path.join(directory, "discutere.db");
for (const suffix of ["", "-shm", "-wal"]) fs.rmSync(`${process.env.DATABASE_PATH}${suffix}`, { force: true });

const { _resetFlowDb } = await import("../../src/flow/db/connection.js");
_resetFlowDb();

const { importVoluptasPersonas, parseVoluptasPersonaPayload } = await import(
  "../../src/flow/persona-import.js"
);
const { findPoolPersonaByUserId, listPoolPersonas, upsertPoolPersonaBySpeaker } = await import(
  "../../src/flow/persona-pool.js"
);
const { DIM } = await import("../../src/flow/sentiment-vector.js");

const userId = "ext:voluptas:0123456789abcdef";
const first = importVoluptasPersonas({
  personas: [{ user_id: userId, affect_vector: new Array(DIM).fill(0.25), vector_spec_version: 1, traits: ["探索型"] }],
});
assert.deepEqual(first, { imported: 1, inserted: 1, updated: 0 });
const persona = findPoolPersonaByUserId(userId);
assert.ok(persona);
assert.equal(persona.origin, "imported");
assert.equal(persona.name.startsWith("プレイヤー#"), true);
assert.equal(persona.name.includes(userId), false);
assert.equal(persona.sourceSpeakerId, `ext:feedback:${userId}`);

const second = importVoluptasPersonas([
  { user_id: userId, affect_vector: new Array(DIM).fill(0.5), vector_spec_version: 1, traits: ["慎重型"] },
]);
assert.deepEqual(second, { imported: 1, inserted: 0, updated: 1 });
assert.equal(listPoolPersonas({ origin: "imported" }).length, 1);
assert.deepEqual(findPoolPersonaByUserId(userId)?.traits, ["慎重型"]);

upsertPoolPersonaBySpeaker({
  id: "adopted-duplicate",
  name: "論者#duplicate",
  role: "opinion",
  speechStyle: "",
  traits: [],
  affectVector: new Array(DIM).fill(-0.5),
  origin: "adopted",
  parentIds: [],
  sourceSpeakerId: `ext:feedback:${userId}`,
  typicality: 0.8,
});
assert.equal(listPoolPersonas().length, 1, "同じ Voluptas 発話話者を二重登録しない");
assert.equal(findPoolPersonaByUserId(userId)?.origin, "imported", "Voluptas profile remains authoritative");
assert.equal(findPoolPersonaByUserId(userId)?.affectVector[0], 0.5, "review adoption does not replace profile vector");

assert.throws(
  () => parseVoluptasPersonaPayload({ user_id: "raw-voluptas-sid", affect_vector: new Array(DIM).fill(0), vector_spec_version: 1 }),
  /HMAC pseudonym/
);
assert.throws(
  () => parseVoluptasPersonaPayload({ user_id: userId, affect_vector: [0], vector_spec_version: 1 }),
  /exactly 20/
);
assert.throws(
  () => parseVoluptasPersonaPayload({ user_id: userId, affect_vector: new Array(DIM).fill(0), vector_spec_version: 2 }),
  /unsupported vector_spec_version/
);

_resetFlowDb();
console.log("  [ok] Voluptas persona import: pseudonymous upsert + fail-fast validation");
