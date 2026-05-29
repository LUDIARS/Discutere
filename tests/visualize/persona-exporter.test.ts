/**
 * persona md exporter テスト (PR-E / MISSING #5-1).
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import matter from "gray-matter";
import Database from "better-sqlite3";

import {
  applyPersonaEngineMigrations,
  DISCUSSION_PERSONA_SEEDS,
  PersonasRepo,
} from "../../src/persona-engine/index.js";
import {
  dumpPersona,
  exportPersona,
  extractWikilinks,
  parseWikilink,
} from "../../src/visualize/index.js";

const workDir = path.resolve(".tmp/vz-persona-exporter");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });
const db = new Database(path.join(workDir, "pe.db"));
applyPersonaEngineMigrations(db);
const personas = new PersonasRepo(db);
personas.bulkSeed(DISCUSSION_PERSONA_SEEDS);

const workspaceId = "knowledge";
const rootDir = path.join(workDir, "discussions");

// 1. exportPersona は markdown を返す
{
  const r = exportPersona(personas, "advocate", workspaceId);
  const parsed = matter(r.markdown);
  assert.equal(parsed.data.id, "advocate");
  assert.equal(parsed.data.type, "persona");
  assert.equal(parsed.data.workspace_id, workspaceId);
  assert.equal(parsed.data.name, "推進派");
  assert.equal(parsed.data.display_name, "押野 進");
  assert.ok(Array.isArray(parsed.data.traits));
  assert.ok(parsed.data.traits.length > 0);
  assert.ok(typeof parsed.data.speech_style === "string");
  assert.ok(r.markdown.includes("# 推進派 (押野 進)"));
  console.log("ok exportPersona frontmatter + body");
}

// 2. dumpPersona は file を書く + created/unchanged 判定
{
  const r1 = dumpPersona(personas, "sceptic", { rootDir, workspaceId });
  assert.equal(r1.written, "created");
  assert.ok(r1.filePath && r1.filePath.endsWith(`${path.sep}persona${path.sep}sceptic.md`));
  assert.ok(fs.existsSync(r1.filePath!));

  const r2 = dumpPersona(personas, "sceptic", { rootDir, workspaceId });
  assert.equal(r2.written, "unchanged");
  console.log("ok dumpPersona created + unchanged");
}

// 3. learned_notes は frontmatter の learned_notes_count + body に表示
{
  personas.appendLearnedNote("refiner", "条件分岐で説得力上がった");
  personas.appendLearnedNote("refiner", "和解案が validated 増えた");
  const r = exportPersona(personas, "refiner", workspaceId);
  const parsed = matter(r.markdown);
  assert.equal(parsed.data.learned_notes_count, 2);
  assert.ok(r.markdown.includes("条件分岐で説得力上がった"));
  console.log("ok learned_notes shown");
}

// 4. wikilink パース: [[persona:advocate]] 形式が parse 可能
{
  const parsed = parseWikilink("[[persona:advocate]]");
  assert.deepEqual(parsed, { type: "persona", id: "advocate" });
  // 本文中の混在
  const links = extractWikilinks("テキスト中 [[persona:advocate]] と [[hyp:abc]] が並ぶ");
  const types = links.map((l) => l.type).sort();
  assert.deepEqual(types, ["hyp", "persona"]);
  console.log("ok [[persona:id]] wikilink parses");
}

// 5. 不存在 persona は throw
{
  assert.throws(() => exportPersona(personas, "ghost-persona-id", workspaceId), /not found/);
  console.log("ok exportPersona throws on missing id");
}

db.close();
console.log("persona-exporter.test.ts: all passed");
