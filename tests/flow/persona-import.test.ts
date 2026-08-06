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
const {
  archivePoolPersona,
  findPoolPersonaByUserId,
  insertPoolPersona,
  listPoolPersonas,
  upsertPoolPersonaBySpeaker,
} = await import(
  "../../src/flow/persona-pool.js"
);
const { estimatePopulationReport } = await import("../../src/flow/persona-populations.js");
const { DIM } = await import("../../src/flow/sentiment-vector.js");

const userId = "ext:voluptas:0123456789abcdef";
const first = importVoluptasPersonas({
  personas: [{ user_id: userId, affect_vector: new Array(DIM).fill(0.25), vector_spec_version: 1, traits: ["探索型"] }],
});
assert.deepEqual(first, {
  imported: 1,
  inserted: 1,
  updated: 0,
  skipped: 0,
  skipReasons: {},
  dryRun: false,
});
const persona = findPoolPersonaByUserId(userId);
assert.ok(persona);
assert.equal(persona.origin, "imported");
assert.equal(persona.name.startsWith("プレイヤー#"), true);
assert.equal(persona.name.includes(userId), false);
assert.equal(persona.sourceSpeakerId, `ext:feedback:${userId}`);

const second = importVoluptasPersonas([
  {
    pseudoId: userId,
    affectVector: new Array(DIM).fill(0.5),
    vectorSpecVersion: 1,
    exportSpecVersion: 2,
    traits: ["慎重型"],
    preferenceAxes: { "style.explorer": 0.9, "style.routine_tolerance": 0.1 },
    attributes: { ageBand: "20s", spending: "light" },
    aversions: [{ target: "mechanic:core/gacha", strength: 0.8 }],
    mechanicReactions: [{ mechanicId: "action/dodge-roll", sentiment: 1.5 }],
  },
]);
assert.deepEqual(second, {
  imported: 1,
  inserted: 0,
  updated: 1,
  skipped: 0,
  skipReasons: {},
  dryRun: false,
});
assert.equal(listPoolPersonas({ origin: "imported" }).length, 1);
assert.deepEqual(findPoolPersonaByUserId(userId)?.traits, ["慎重型"]);
assert.deepEqual(findPoolPersonaByUserId(userId)?.preferenceAxes, {
  "style.explorer": 0.9,
  "style.routine_tolerance": 0.1,
});
assert.deepEqual(findPoolPersonaByUserId(userId)?.attributes, {
  ageBand: "20s",
  spending: "light",
});

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

// user_id の unique index はアーカイブ行にも効くので、再 export は復帰 (更新) でなければならない。
// archived を無視して INSERT すると UNIQUE 制約で取込全体が落ちる。
const archivedPersona = findPoolPersonaByUserId(userId);
assert.ok(archivedPersona);
archivePoolPersona(archivedPersona.id);
assert.equal(findPoolPersonaByUserId(userId), null, "アーカイブ後は通常検索から消える");
const revived = importVoluptasPersonas([{
  pseudoId: userId,
  affectVector: new Array(DIM).fill(0.5),
  vectorSpecVersion: 1,
  exportSpecVersion: 2,
  traits: ["慎重型"],
  preferenceAxes: { "style.explorer": 0.9, "style.routine_tolerance": 0.1 },
  attributes: { ageBand: "20s", spending: "light" },
  aversions: [{ target: "mechanic:core/gacha", strength: 0.8 }],
  mechanicReactions: [{ mechanicId: "action/dodge-roll", sentiment: 1.5 }],
}]);
assert.deepEqual(revived, {
  imported: 1,
  inserted: 0,
  updated: 1,
  skipped: 0,
  skipReasons: {},
  dryRun: false,
});
assert.equal(findPoolPersonaByUserId(userId)?.id, archivedPersona.id, "同じ行を復帰させる");
assert.equal(listPoolPersonas({ origin: "imported" }).length, 1, "復帰で行を増やさない");

assert.throws(() => parseVoluptasPersonaPayload({
  user_id: "raw-voluptas-sid",
  affect_vector: new Array(DIM).fill(0),
  vector_spec_version: 1,
}), /HMAC pseudonym/);
assert.throws(
  () => parseVoluptasPersonaPayload({ user_id: userId, affect_vector: [0], vector_spec_version: 1 }),
  /exactly 20/
);
assert.throws(
  () => parseVoluptasPersonaPayload({ user_id: userId, affect_vector: new Array(DIM).fill(0), vector_spec_version: 2 }),
  /unsupported vectorSpecVersion/
);

const skipped = importVoluptasPersonas([
  { pseudoId: "raw", affectVector: new Array(DIM).fill(0), vectorSpecVersion: 1, exportSpecVersion: 2 },
]);
assert.equal(skipped.skipped, 1);
assert.deepEqual(skipped.skipReasons, { "pseudo-id": 1 });

const dry = importVoluptasPersonas([{
  pseudoId: "ext:voluptas:1111111111111111",
  affectVector: new Array(DIM).fill(0.2),
  vectorSpecVersion: 1,
  exportSpecVersion: 2,
}], { dryRun: true });
assert.equal(dry.inserted, 1);
assert.equal(dry.dryRun, true);
assert.equal(findPoolPersonaByUserId("ext:voluptas:1111111111111111"), null);

insertPoolPersona({
  id: "adopted-neighbor",
  name: "論者#neighbor",
  role: "opinion",
  speechStyle: "",
  traits: [],
  affectVector: new Array(DIM).fill(0.5),
  origin: "adopted",
  parentIds: [],
});
const population = estimatePopulationReport({
  similarityThreshold: 0.9,
  majorRatio: 0.5,
  generatedAt: "2026-07-28T00:00:00.000Z",
});
assert.deepEqual(population, {
  generatedAt: "2026-07-28T00:00:00.000Z",
  realPopulation: 1,
  entries: [{
    pseudoId: userId,
    verdict: "major",
    ratio: 1,
    nearestClusterSize: 1,
  }],
});

_resetFlowDb();
console.log("  [ok] Voluptas persona import: pseudonymous upsert + fail-fast validation");
