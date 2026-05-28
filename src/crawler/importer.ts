/**
 * Crawler importer (Phase 0).
 *
 * GameKG (md frontmatter) を Discatier Core の Game / Mechanic / Aesthetic
 * ノードに append する。 既存 Game は title 一致で upsert、
 * Mechanic / Aesthetic は (game_id, name) 一致で upsert する。
 *
 * intends / intended_affect は Mechanic 既存カラム (phase 2/4 migration)
 * に直接 UPDATE する。 Discatier の MechanicProposed イベントとは別ルートで、
 * crawl 由来であることを示すため event_type "MechanicCrawlImported" は
 * 立てない (= MechanicProposed として扱われる)。 Phase 2 で event 化検討。
 */

import type { createCore } from "../core/index.js";

import type { GameKG } from "./types.js";

type Core = ReturnType<typeof createCore>;

export interface ImportResult {
  gameId: string;
  inserted: { mechanics: number; aesthetics: number };
  updated: { mechanics: number; aesthetics: number; game: boolean };
}

export function importGameKG(core: Core, kg: GameKG): ImportResult {
  const ws = kg.workspace_id;

  const existingGame = findGameByTitle(core, ws, kg.title);
  let gameId: string;
  let gameUpdated = false;
  if (existingGame) {
    gameId = existingGame.id;
    if (existingGame.genre !== (kg.genre ?? null) || existingGame.title !== kg.title) {
      core.repos.game.update(gameId, { workspaceId: ws, title: kg.title, genre: kg.genre });
      gameUpdated = true;
    }
  } else {
    gameId = core.repos.game.create({ workspaceId: ws, title: kg.title, genre: kg.genre });
  }

  let mInserted = 0;
  let mUpdated = 0;
  for (const m of kg.mechanics) {
    const existing = findMechanicByName(core, ws, gameId, m.name);
    if (existing) {
      core.repos.mechanic.update(existing.id, {
        workspaceId: ws,
        gameId,
        name: m.name,
        description: m.description,
      });
      applyMechanicExtras(core, existing.id, m.intends, m.intended_affect);
      mUpdated += 1;
    } else {
      const id = core.repos.mechanic.create({
        workspaceId: ws,
        gameId,
        name: m.name,
        description: m.description,
      });
      applyMechanicExtras(core, id, m.intends, m.intended_affect);
      // create 時は events log 経由のため、 mechanics table への row 挿入は
      // projection 側に任せず raw insert で揃える (phase 0 の単純化)。
      ensureMechanicRow(core, id, ws, gameId, m.name, m.description);
      mInserted += 1;
    }
  }

  let aInserted = 0;
  let aUpdated = 0;
  for (const a of kg.aesthetics) {
    const existing = findAestheticByName(core, ws, gameId, a.name);
    if (existing) {
      core.repos.aesthetic.update(existing.id, {
        name: a.name,
        description: a.description,
      });
      aUpdated += 1;
    } else {
      core.repos.aesthetic.create({
        workspaceId: ws,
        gameId,
        name: a.name,
        description: a.description,
      });
      aInserted += 1;
    }
  }

  return {
    gameId,
    inserted: { mechanics: mInserted, aesthetics: aInserted },
    updated: { mechanics: mUpdated, aesthetics: aUpdated, game: gameUpdated },
  };
}

interface GameRow {
  id: string;
  workspace_id: string;
  title: string;
  genre: string | null;
}
interface MechanicRow {
  id: string;
  workspace_id: string;
  game_id: string | null;
  name: string;
  description: string | null;
  intends: string | null;
  intended_affect: string | null;
}
interface AestheticRow {
  id: string;
  workspace_id: string;
  game_id: string | null;
  name: string;
  description: string | null;
}

function findGameByTitle(core: Core, ws: string, title: string): GameRow | undefined {
  return core.client.raw
    .prepare("SELECT * FROM games WHERE workspace_id = ? AND title = ? LIMIT 1")
    .get(ws, title) as GameRow | undefined;
}

function findMechanicByName(
  core: Core,
  ws: string,
  gameId: string,
  name: string
): MechanicRow | undefined {
  return core.client.raw
    .prepare(
      "SELECT * FROM mechanics WHERE workspace_id = ? AND game_id = ? AND name = ? LIMIT 1"
    )
    .get(ws, gameId, name) as MechanicRow | undefined;
}

function findAestheticByName(
  core: Core,
  ws: string,
  gameId: string,
  name: string
): AestheticRow | undefined {
  return core.client.raw
    .prepare(
      "SELECT * FROM aesthetics WHERE workspace_id = ? AND game_id = ? AND name = ? LIMIT 1"
    )
    .get(ws, gameId, name) as AestheticRow | undefined;
}

function applyMechanicExtras(
  core: Core,
  id: string,
  intends: string | undefined,
  intendedAffect: string | undefined
): void {
  if (intends === undefined && intendedAffect === undefined) return;
  core.client.raw
    .prepare(
      "UPDATE mechanics SET intends = COALESCE(?, intends), intended_affect = COALESCE(?, intended_affect), updated_at = ? WHERE id = ?"
    )
    .run(intends ?? null, intendedAffect ?? null, Date.now(), id);
}

/**
 * Mechanic create は event 経由なので projection 側でしか mechanics row を作らない。
 * Phase 0 importer は projection 走行を待たず即時参照したいので raw insert で揃える。
 */
function ensureMechanicRow(
  core: Core,
  id: string,
  ws: string,
  gameId: string,
  name: string,
  description: string | undefined
): void {
  const existing = core.client.raw
    .prepare("SELECT 1 FROM mechanics WHERE id = ?")
    .get(id) as { 1: number } | undefined;
  if (existing) return;
  const at = Date.now();
  core.client.raw
    .prepare(
      "INSERT INTO mechanics (id, workspace_id, game_id, name, description, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?)"
    )
    .run(id, ws, gameId, name, description ?? null, at, at);
}
