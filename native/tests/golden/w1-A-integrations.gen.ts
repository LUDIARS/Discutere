// src/integrations/conclusion-publication.ts の入出力を記録する。
// 実行: npx tsx native/tests/golden/w1-A-integrations.gen.ts
// 合成の SQLite (DDL は src/flow/db/migrations.ts の写し) に対する操作列と戻り値。
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";
import { publicationState, publishedConclusions, setPublication } from "../../../src/integrations/conclusion-publication.js";

const ddl = [
  `CREATE TABLE conclusion_cache (gap_id TEXT PRIMARY KEY, kind TEXT NOT NULL, session_id TEXT NOT NULL, title TEXT NOT NULL DEFAULT '', conclusion TEXT, utterance_count INTEGER NOT NULL DEFAULT 0, aufhebung_count INTEGER NOT NULL DEFAULT 0, discussion_volume INTEGER NOT NULL DEFAULT 0, material_count INTEGER NOT NULL DEFAULT -1, updated_at INTEGER NOT NULL, cached_at INTEGER NOT NULL)`,
  `CREATE TABLE conclusion_publication (conclusion_id TEXT PRIMARY KEY, publishable INTEGER NOT NULL DEFAULT 0 CHECK(publishable IN (0,1)), scope TEXT NOT NULL DEFAULT 'private' CHECK(scope IN ('private','public','glab')), content_hash TEXT NOT NULL, updated_at INTEGER NOT NULL)`,
];
const rows = [
  `INSERT INTO conclusion_cache (gap_id,kind,session_id,title,conclusion,updated_at,cached_at) VALUES ('public','gap','s','議題 "公開"','結論です',5,0)`,
  `INSERT INTO conclusion_cache (gap_id,kind,session_id,title,conclusion,updated_at,cached_at) VALUES ('glab','gap','s','glab','approved',7,0)`,
  `INSERT INTO conclusion_cache (gap_id,kind,session_id,title,conclusion,updated_at,cached_at) VALUES ('private','gap','s','private','approved',3,0)`,
  `INSERT INTO conclusion_cache (gap_id,kind,session_id,title,conclusion,updated_at,cached_at) VALUES ('blank','gap','s','blank','   ',9,0)`,
  `INSERT INTO conclusion_cache (gap_id,kind,session_id,title,conclusion,updated_at,cached_at) VALUES ('null','gap','s','null',NULL,9,0)`,
];
const db = new Database(":memory:");
for (const s of [...ddl, ...rows]) db.exec(s);
const steps: unknown[] = [];
const rev = (id: string) => publicationState(db, id)?.revision ?? "";
const log = (op: string, args: unknown, result: unknown) => steps.push({ op, args, result });
for (const id of ["public", "glab", "private", "blank", "null", "missing"]) log("state", { id }, publicationState(db, id));
log("published", {}, publishedConclusions(db));
for (const [id, scope] of [["public", "public"], ["glab", "glab"], ["private", "private"], ["blank", "public"], ["null", "public"]]) {
  const args = { id, publishable: true, scope, revision: rev(id), now: 11 };
  log("set", args, setPublication(db, id, true, scope as never, args.revision, 11));
}
log("set", { id: "glab", publishable: true, scope: "glab", revision: "wrong", now: 12 }, setPublication(db, "glab", true, "glab", "wrong", 12));
log("set", { id: "blank", publishable: false, scope: "public", revision: rev("blank"), now: 12 }, setPublication(db, "blank", false, "public", rev("blank"), 12));
log("published", {}, publishedConclusions(db));
log("published", { id: "public" }, publishedConclusions(db, "public"));
log("exec", { sql: "UPDATE conclusion_cache SET conclusion='changed' WHERE gap_id='public'" }, db.prepare("UPDATE conclusion_cache SET conclusion='changed' WHERE gap_id='public'").run().changes);
log("state", { id: "public" }, publicationState(db, "public"));
log("published", {}, publishedConclusions(db));
log("set", { id: "glab", publishable: false, scope: "glab", revision: rev("glab"), now: 13 }, setPublication(db, "glab", false, "glab", rev("glab"), 13));
log("published", {}, publishedConclusions(db));
const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "integrations");
fs.writeFileSync(path.join(dir, "conclusion_publication.json"), JSON.stringify({ sql: [...ddl, ...rows], steps }, null, 2) + "\n");
console.log(`wrote ${steps.length} steps`);
