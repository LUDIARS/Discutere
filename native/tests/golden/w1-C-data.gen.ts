// データ層 (core / db / flow/db / kg-sync / discatier-engine-adapter) の Node 実装の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-C-data.gen.ts
// 出力: native/tests/fixtures/{core,flow_db,db,discatier_engine_adapter,kg_sync}/*.json
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import Database from "better-sqlite3";

import { applyMigrations } from "../../../src/core/db/schema.js";
import { applyFlowMigrations } from "../../../src/flow/db/migrations.js";
import * as drizzleSchema from "../../../src/db/schema.js";
import { parseTitleAliases, buildAliasGroups, expandAliases, STATIC_ALIAS_GROUPS } from "../../../src/discatier-engine-adapter/game-aliases.js";
import { extractKeyTerms } from "../../../src/discatier-engine-adapter/keyword-terms.js";
import { LUDUS_TERM_ALIAS_GROUPS } from "../../../src/discatier-engine-adapter/ludus-term-aliases.js";
import { rrfFuse, mmrSelect, dedupeNearDuplicates, bigramJaccardSimilarity } from "../../../src/core/vectors/hybrid-rank.js";
import { cosineSimilarity, isEmbeddingVector } from "../../../src/core/vectors/vector-math.js";
import { parseCommand } from "../../../src/core/projection/command-parser.js";
import { inferRespondsTo } from "../../../src/core/projection/responds-to-inference.js";
import { canTransition } from "../../../src/core/hypothesis/state-machine.js";
import { applyEventProjection } from "../../../src/core/events/projection.js";
import { EVENT_TYPES } from "../../../src/core/events/event-types.js";
import { exportSharedKg } from "../../../src/kg-sync/export.js";
import { importSharedKg } from "../../../src/kg-sync/import.js";

const require = createRequire(import.meta.url);
const root = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures");
const write = (rel: string, value: unknown) => {
  const file = path.join(root, rel);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(value, null, 2) + "\n");
  console.log(`wrote ${rel}`);
};
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "w1c-golden-"));

// ── migration の文を記録する (偽の db で exec された文を順に集める) ──
function captureMigrations(apply: (db: any) => void): Array<{ id: string; sql: string[] }> {
  const out: Array<{ id: string; sql: string[] }> = [];
  let pending: string[] = [];
  let started = false;
  const fake = {
    exec(sql: string) {
      if (sql === "BEGIN") { started = true; return; }
      if (sql === "COMMIT" || sql === "ROLLBACK") return;
      if (started) pending.push(sql);
    },
    prepare(_sql: string) {
      return {
        get: () => undefined,
        run: (id: string) => { out.push({ id, sql: pending }); pending = []; },
      };
    },
  };
  apply(fake);
  return out;
}

function master(db: Database.Database) {
  return db.prepare("SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY type, name").all();
}

{
  const migrations = captureMigrations(applyMigrations);
  const db = new Database(path.join(tmp, "core.db"));
  db.pragma("journal_mode = WAL");
  db.pragma("foreign_keys = ON");
  applyMigrations(db);
  write("core/schema.json", { migrations, sqlite_master: master(db) });
  db.close();
}
{
  const migrations = captureMigrations(applyFlowMigrations);
  const db = new Database(path.join(tmp, "flow.db"));
  applyFlowMigrations(db);
  write("flow_db/migrations.json", { migrations, sqlite_master: master(db) });
  db.close();
}
{
  const api = require("drizzle-kit/api");
  const empty = await api.generateSQLiteDrizzleJson({});
  const cur = await api.generateSQLiteDrizzleJson(drizzleSchema);
  const statements: string[] = await api.generateSQLiteMigration(empty, cur);
  const db = new Database(path.join(tmp, "main.db"));
  for (const s of statements) db.exec(s);
  write("db/schema.json", { statements, sqlite_master: master(db) });
  db.close();
}

// ── 純関数 ──
{
  const titles = [
    "Genshin Impact (原神)", "Shadow of the Colossus (ワンダと巨像)", "モンスターストライク", "Hollow Knight",
    "Elden Ring（エルデンリング）", "A (B) (C)", "x()", "(only inner)", "ab（", "Title (", "（全角）タイトル", "Q ()(inner) z",
  ];
  const groups = buildAliasGroups(titles);
  const groupsExtra = buildAliasGroups(titles, LUDUS_TERM_ALIAS_GROUPS);
  const extraOnly = buildAliasGroups(["モンスターストライク"], [["経験値", "XP", "EXP"], ["単独語のみ"], ["a", "bb", "BB", " cc "]]);
  const termsCases = [
    ["モンスターストライク"], ["モンスト"], ["原神"], ["無関係なテーマ"], ["モンスト", "周回"], ["Genshin"], ["  ELDEN ring "],
    ["経験値"], ["xp"], ["ヒット"], ["ヒットボックス判定"], ["ab", "abc", "abcd"], [""], ["death"], ["デススト"],
  ];
  write("discatier_engine_adapter/game_aliases.json", {
    staticGroups: STATIC_ALIAS_GROUPS,
    ludusGroups: LUDUS_TERM_ALIAS_GROUPS,
    parse: titles.map((t) => ({ title: t, out: parseTitleAliases(t) })),
    titles,
    groups,
    groupsExtra,
    extraOnly,
    expand: termsCases.flatMap((terms) => [
      { terms, which: "groups", max: 12, out: expandAliases(terms, groups) },
      { terms, which: "groupsExtra", max: 12, out: expandAliases(terms, groupsExtra) },
      { terms, which: "groupsExtra", max: 3, out: expandAliases(terms, groupsExtra, 3) },
      { terms, which: "extraOnly", max: 12, out: expandAliases(terms, extraOnly) },
    ]),
  });
  const keyCases = [
    ["モンスターストライクの引っ張りアクションは面白いか"], ["ボス戦"], [""], ["の は が"], ["Elden Ring の評価"],
    ["あ あ あ", "ボス戦 ボス戦"], ["ゲームについて、なぜ面白いのか？"], ["『ゼルダの伝説』のダンジョン設計 (謎解き)"],
    ["ハンターハンターとかけど、からはまでに"], ["ＡＢＣ　ＤＥＦ"], ["ÀÉÎ Straße"], ["a.b,c;d:e/f|g~h…i"],
    ["😀絵文字のテスト😀"], ["ガチャ排出率でしょうか報酬設計ですか"], ["x".repeat(40) + " " + "y".repeat(10)],
    ["いろはにほへと"], ["戦闘システムやUIなどの改善"],
  ];
  write("discatier_engine_adapter/keyword_terms.json", keyCases.flatMap((terms) => [
    { terms, max: 6, out: extractKeyTerms(terms) },
    { terms, max: 2, out: extractKeyTerms(terms, 2) },
  ]));

  const rrfCases = [
    { rankings: [["a", "b", "c"], ["b", "a", "d"]] },
    { rankings: [["a", "b"], ["b", "a"]], weights: [0, 1] },
    { rankings: [["x", "y", "x"], [], ["z"]], weights: [0.5] },
  ];
  const mmrCases = [
    { candidates: [{ id: "v1", relevance: 1.0, vector: [1, 0] }, { id: "v2", relevance: 0.9, vector: [0.999, 0.01] }, { id: "v3", relevance: 0.5, vector: [0, 1] }], k: 2, lambda: 0.5 },
    { candidates: [{ id: "v1", relevance: 1.0, vector: [1, 0] }, { id: "n1", relevance: 0.8 }, { id: "n2", relevance: 0.2 }], k: 2 },
    { candidates: [{ id: "keyword-top", relevance: 1.0 }, { id: "embedded-low", relevance: 0.8, vector: [1, 0] }], k: 1 },
    { candidates: [], k: 3 },
    { candidates: [{ id: "a", relevance: 1, vector: [1] }], k: 0 },
    { candidates: [{ id: "a", relevance: 0.5, vector: [1, 2, 3] }, { id: "b", relevance: 0.5, vector: [3, 2, 1] }, { id: "c", relevance: 0.7, vector: [1, 2] }, { id: "d", relevance: 0.1, vector: [-1, -2, -3] }, { id: "e", relevance: 0.5 }], k: 10, lambda: 0.3 },
  ];
  const dedupeItems = [
    { id: "a", content: "このゲームの引っ張りアクションが最高に気持ちいい" },
    { id: "b", content: "このゲームの引っ張りアクションが最高に気持ちいい!" },
    { id: "c", content: "ガチャの排出率が渋すぎて課金する気になれない" },
    { id: "d", content: "  GACHA   Rates  " },
    { id: "e", content: "gacha rates" },
    { id: "f", content: "" },
    { id: "g", content: "あ" },
    { id: "h", content: "😀😀😀絵文字" },
    { id: "i", content: "😀😀😀絵文字!" },
  ];
  const pairs = [["abc", "abd"], ["", "x"], ["😀a", "😀b"], ["Hello World", "hello   world"], ["ガチャ", "ガチャガチャ"]];
  const vecs = [[[1, 0], [0, 1]], [[1, 2, 3], [1, 2, 3]], [[1, 2], [1, 2, 3]], [[], []], [[0, 0], [1, 1]], [[0.3, -0.7, 1e-9], [2, 5, -1]]];
  write("core/hybrid_rank.json", {
    rrf: rrfCases.map((c) => ({ ...c, out: [...rrfFuse(c.rankings, c.weights).entries()] })),
    mmr: mmrCases.map((c) => ({ ...c, out: mmrSelect(c.candidates as any, c.k, c.lambda).map((x) => x.id) })),
    dedupe: [0.82, 0.5, 0.999].map((threshold) => ({ threshold, out: dedupeNearDuplicates(dedupeItems, threshold).map((x) => x.id) })),
    dedupeItems,
    jaccard: pairs.map(([a, b]) => ({ a, b, out: bigramJaccardSimilarity(a, b) })),
    cosine: vecs.map(([a, b]) => ({ a, b, out: cosineSimilarity(a, b) })),
    isVector: [[1, 2], [], [1, "x"], "x", null, [0.5]].map((v) => ({ v, out: isEmbeddingVector(v) })),
  });

  const commands = ["hello", "/define Jump Arc", " /SUBMIT   ", "/", "/   ", "/a\tb\nc", "//x", "/ÀB c", "  /x  y  "];
  const responds = ["> @uid:abc.def-1 hi", ">@uid:X", "  > @uid:a:b_c\nnext", "text\n> @uid:x", "> @uid:", "> @uid:日本", "\r\n> @uid:x", "> @uid:ok!rest"];
  const states = ["draft", "proposed", "under_theory_validation", "validated_by_theory", "under_emotion_validation", "validated_by_emotion", "integrated", "rejected"];
  const actions = ["submit", "validate_theory", "validate_emotion", "session_end", "integrate", "reject", "bogus"];
  write("core/pure.json", {
    commands: commands.map((c) => ({ in: c, out: parseCommand(c) })),
    responds: responds.map((c) => ({ in: c, out: inferRespondsTo(c) })),
    transitions: states.flatMap((s) => actions.map((a) => ({ from: s, action: a, out: canTransition(s as any, a as any) }))),
  });
}

// ── 射影 (events → SQLite) ──
{
  const db = new Database(":memory:");
  applyMigrations(db);
  const ev = (eventType: string, payload: Record<string, unknown>, id: string, createdAt = 1000) => ({ id, eventType, payload, createdAt });
  const events = [
    ev(EVENT_TYPES.PersonCreated, { workspaceId: "ws", id: "p1", name: "alice" }, "e1"),
    ev(EVENT_TYPES.PersonUpdated, { workspaceId: "ws", id: "p1", name: "alice2", role: "gm", at: 2000 }, "e2"),
    ev(EVENT_TYPES.SessionCreated, { workspaceId: "ws", id: "s1", title: "t", startedAt: 5, mode: "discussion", scene: "web:r" }, "e3"),
    ev(EVENT_TYPES.SessionEnded, { workspaceId: "ws", id: "s1", endedAt: 99 }, "e4", 3000),
    ev(EVENT_TYPES.UtteranceCreated, { workspaceId: "ws", id: "u1", sessionId: "s1", rawContent: "hello", postedAt: 7, respondsTo: "u0" }, "e5"),
    ev(EVENT_TYPES.UtteranceNormalized, { workspaceId: "ws", id: "u1", normalizedContent: "hello!" }, "e6", 4000),
    ev(EVENT_TYPES.ReactionAdded, { workspaceId: "ws", id: "r1", utteranceId: "u1", reactionType: "like", intensity: 0.5 }, "e7"),
    ev(EVENT_TYPES.MechanicProposed, { workspaceId: "ws", id: "m1", name: "gacha", intends: "fun" }, "e8"),
    ev(EVENT_TYPES.AffectMeasured, { workspaceId: "ws", id: "a1", mood: "happy", score: 1.25 }, "e9"),
    ev(EVENT_TYPES.PlayContextCaptured, { workspaceId: "ws", id: "c1", platform: "pc" }, "e10"),
    ev(EVENT_TYPES.DesignGapDetected, { workspaceId: "ws", id: "g1", title: "gap", status: "open" }, "e11"),
    ev(EVENT_TYPES.HypothesisProposed, { workspaceId: "ws", id: "h1", statement: "st", refs: ["a", "b"], integrated: true }, "e12"),
    ev(EVENT_TYPES.HypothesisDebated, { workspaceId: "ws", id: "h1", statement: "st2", sessionId: "s1", status: "debating", staleFlagged: true, validatedByEmotion: 1 }, "e13"),
    ev(EVENT_TYPES.HypothesisStaleFlagged, { workspaceId: "ws", id: "h1", statement: "st3", staleFlagged: false }, "e14"),
    ev(EVENT_TYPES.AffectAdded, { workspaceId: "ws", id: "ignored", mood: "x" }, "e15"),
  ];
  for (const e of events) applyEventProjection(db, e as any);
  const tables = ["people", "sessions", "utterances", "reactions", "mechanics", "affects", "play_contexts", "design_gaps", "hypotheses", "hypothesis_validations"];
  const rows: Record<string, unknown[]> = {};
  for (const t of tables) rows[t] = db.prepare(`SELECT * FROM ${t} ORDER BY id`).all();
  write("core/projection.json", { events, rows });
  db.close();
}

// ── kg-sync ──
{
  const db = new Database(":memory:");
  applyMigrations(db);
  const ins = db.prepare(`INSERT INTO games (id, workspace_id, title, genre, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?, ?)`);
  ins.run("g1", "knowledge", "Vampire \"Survivors\"\n改行", null, 1000, 1000, 1);
  ins.run("g2", "knowledge", "Hades", "rogue", 2000, 2500, 1);
  ins.run("g3", "other", "Other", null, 3000, 3000, 1);
  ins.run("g4", "knowledge", "private", null, 3000, 4000, 0);
  db.prepare(`INSERT INTO mechanics (id, workspace_id, game_id, name, description, created_at, updated_at, shared, intends) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`)
    .run("m1", "knowledge", "g1", "経験値磁石", "<desc>", 1500, 1500, 1, "x");
  db.prepare(`INSERT INTO affects (id, workspace_id, mood, score, created_at, updated_at, shared, valence) VALUES (?, ?, ?, ?, ?, ?, ?, ?)`)
    .run("af1", "knowledge", "joy", 0.1 + 0.2, 10, 3500, 1, "pos");
  db.prepare(`INSERT INTO aesthetics (id, workspace_id, name, created_at, updated_at, shared) VALUES (?, ?, ?, ?, ?, ?)`)
    .run("ae1", "knowledge", "美", 1, 1e15, 1);
  const exports = [
    { opts: {}, ...exportSharedKg(db, {}) },
    { opts: { since: 1500 }, ...exportSharedKg(db, { since: 1500 }) },
    { opts: { since: 0, workspaceId: "knowledge" }, ...exportSharedKg(db, { since: 0, workspaceId: "knowledge" }) },
    { opts: { since: 99999999999999999 }, ...exportSharedKg(db, { since: 1e17 }) },
  ];
  // import の ndjson と結果
  const follower = new Database(":memory:");
  applyMigrations(follower);
  const imports: unknown[] = [];
  const lines = [
    exports[0].ndjson,
    JSON.stringify({ type: "row", table: "games", data: { id: "g2", workspace_id: "knowledge", title: "old", created_at: 1, updated_at: 1, shared: 1 } }),
    JSON.stringify({ type: "row", table: "games", data: { id: "g2", workspace_id: "knowledge", title: "newer", created_at: 1, updated_at: 9000, shared: true, extra_col: "x" } }),
    JSON.stringify({ type: "row", table: "sessions", data: { id: "s", workspace_id: "w", title: "x", started_at: 1, created_at: 1, updated_at: 1 } }),
    "not json\n{\"type\":\"manifest\"}\n\n  \r\n{\"table\":\"games\"}\n{\"table\":\"games\",\"data\":{\"id\":\"\"}}\n{\"table\":\"games\",\"data\":{\"id\":5}}\n[1,2]\nnull\n{\"table\":\"games\",\"data\":{\"id\":\"g9\",\"workspace_id\":\"k\",\"title\":\"t\",\"created_at\":1,\"updated_at\":\"77\",\"shared\":1}}",
    JSON.stringify({ type: "row", table: "mechanics", data: { id: "m2", workspace_id: "k", name: "n", created_at: 1, updated_at: 5, shared: 1, description: undefined } }),
  ];
  lines.push("{\"table\":\"games\",\"data\":{\"id\":\"g10\",\"workspace_id\":\"k\",\"title\":\"t\",\"created_at\":1,\"updated_at\":2,\"shared\":1}}\nnull");
  lines.push(JSON.stringify({ type: "row", table: "games", data: { id: "g11", workspace_id: "k", title: { nested: 1 }, created_at: 1, updated_at: 2, shared: 1 } }));
  for (const nd of lines) {
    try {
      imports.push({ ndjson: nd, result: importSharedKg(follower, nd) });
    } catch (e) {
      imports.push({ ndjson: nd, error: true });
    }
  }
  const state: Record<string, unknown[]> = {};
  for (const t of ["games", "mechanics", "aesthetics", "affects"]) state[t] = follower.prepare(`SELECT * FROM ${t} ORDER BY id`).all();
  write("kg_sync/kg_sync.json", { exports, imports, follower: state });
}

fs.rmSync(tmp, { recursive: true, force: true });
