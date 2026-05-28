/**
 * GraphDB から取るテスト (ユーザ明示要件)
 *
 * md import 後に Discatier Core のリポジトリ API + raw SQL の両方で
 * Game / Mechanic / Aesthetic + edge (mechanic.game_id === game.id) を
 * 取得し、 md と一致することを検証する。
 *
 * 「KG として正しく繋がっているか」 を E2E で見るテスト。
 */

import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

import { createCore } from "../../src/core/index.js";
import { importGameKG, parseGameKG } from "../../src/crawler/index.js";

const workDir = path.resolve(".tmp/crawler-graphdb-read");
fs.rmSync(workDir, { recursive: true, force: true });
fs.mkdirSync(workDir, { recursive: true });

const SAMPLE = `---
id: graph-game-a
title: "Graph Game A"
genre: "RPG"
workspace_id: "knowledge"
sources:
  - url: "https://example.com/a"
    excerpt_policy: "summary-only"
mechanics:
  - name: "Cast Spell"
    description: "MP を消費して魔法を撃つ"
    intends: "リソース管理の戦術判断"
    intended_affect: "tension"
  - name: "Block"
    description: "次の被弾を半減"
aesthetics:
  - name: "中世ハイファンタジー"
    description: "石造城・銀甲冑・古ラテン語"
---
`;

const SAMPLE_B = `---
id: graph-game-b
title: "Graph Game B"
genre: "RPG"
workspace_id: "knowledge"
mechanics:
  - name: "Cast Spell"
    description: "別ゲームでも同名のメカニクス"
---
`;

const core = createCore(path.join(workDir, "db.kuzu"), path.join(workDir, "events.jsonl"));
try {
  const ws = "knowledge";
  const kgA = parseGameKG(SAMPLE);
  const kgB = parseGameKG(SAMPLE_B);
  const resA = importGameKG(core, kgA);
  const resB = importGameKG(core, kgB);
  assert.notEqual(resA.gameId, resB.gameId);

  // ── A. リポジトリ API 経由 (createGameRepo.list / createMechanicRepo.list) ──
  const games = core.repos.game.list(ws) as Array<{
    id: string;
    title: string;
    genre: string | null;
  }>;
  assert.equal(games.length, 2);
  const titles = games.map((g) => g.title).sort();
  assert.deepEqual(titles, ["Graph Game A", "Graph Game B"]);
  console.log("ok repo.game.list");

  // ── B. raw SQL で edge を直接クエリ ──
  // (mechanic) -[game_id]-> (game) の結合
  const joined = core.client.raw
    .prepare(
      `SELECT m.name AS mechanic_name, g.title AS game_title
         FROM mechanics m
         JOIN games g ON g.id = m.game_id
         WHERE m.workspace_id = ?
         ORDER BY g.title, m.name`
    )
    .all(ws) as Array<{ mechanic_name: string; game_title: string }>;
  assert.equal(joined.length, 3); // A: 2, B: 1
  assert.deepEqual(joined, [
    { mechanic_name: "Block", game_title: "Graph Game A" },
    { mechanic_name: "Cast Spell", game_title: "Graph Game A" },
    { mechanic_name: "Cast Spell", game_title: "Graph Game B" },
  ]);
  console.log("ok edge query (mechanic → game)");

  // ── C. cross-game query: 同名 Mechanic を持つ Game を列挙 ──
  // KG の「複数ゲームに渡る Mechanic」 のシンプル例
  const shared = core.client.raw
    .prepare(
      `SELECT m.name AS mechanic_name, COUNT(DISTINCT m.game_id) AS game_count
         FROM mechanics m
         WHERE m.workspace_id = ?
         GROUP BY m.name
         HAVING game_count > 1`
    )
    .all(ws) as Array<{ mechanic_name: string; game_count: number }>;
  assert.equal(shared.length, 1);
  assert.equal(shared[0].mechanic_name, "Cast Spell");
  assert.equal(shared[0].game_count, 2);
  console.log("ok cross-game shared mechanic");

  // ── D. Mechanic.intends / intended_affect が phase 2/4 カラムに入っているか ──
  const cast = core.client.raw
    .prepare(
      `SELECT intends, intended_affect FROM mechanics
         WHERE workspace_id = ? AND game_id = ? AND name = ?`
    )
    .get(ws, resA.gameId, "Cast Spell") as { intends: string | null; intended_affect: string | null };
  assert.equal(cast.intends, "リソース管理の戦術判断");
  assert.equal(cast.intended_affect, "tension");
  console.log("ok mechanic extras (intends / intended_affect)");

  // ── E. Aesthetic も同様に edge を持つ ──
  const aesthetics = core.client.raw
    .prepare(
      `SELECT a.name, g.title AS game_title
         FROM aesthetics a
         JOIN games g ON g.id = a.game_id
         WHERE a.workspace_id = ? AND g.id = ?`
    )
    .all(ws, resA.gameId) as Array<{ name: string; game_title: string }>;
  assert.equal(aesthetics.length, 1);
  assert.equal(aesthetics[0].name, "中世ハイファンタジー");
  assert.equal(aesthetics[0].game_title, "Graph Game A");
  console.log("ok aesthetic edge");

  // ── F. Genre は現状 string カラム (Phase 2 でノード化予定) ──
  const sameGenre = core.client.raw
    .prepare(
      `SELECT title FROM games WHERE workspace_id = ? AND genre = ? ORDER BY title`
    )
    .all(ws, "RPG") as Array<{ title: string }>;
  assert.deepEqual(
    sameGenre.map((r) => r.title),
    ["Graph Game A", "Graph Game B"]
  );
  console.log("ok genre filter");
} finally {
  core.close();
}

console.log("graphdb-read.test.ts: all passed");
