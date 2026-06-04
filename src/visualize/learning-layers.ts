import fs from "node:fs";
import path from "node:path";
import type Database from "better-sqlite3";

import { buildTopicOpinionSnapshot } from "./topic-opinions.js";

export type LearningLayer = "knowledge" | "games" | "opinions";

export interface KnowledgeAffectEntry {
  key: string;
  labelJa: string | null;
  valence: string | null;
  mda: string | null;
  description: string | null;
  usageCount: number;
  size: number;
}

export interface KnowledgeLayerSnapshot {
  layer: "knowledge";
  workspaceId: string;
  generatedAt: number;
  totals: {
    affects: number;
    mechanics: number;
    aesthetics: number;
    games: number;
    gaps: number;
    hypotheses: number;
  };
  affects: KnowledgeAffectEntry[];
}

export interface GameLearningEntry {
  id: string;
  title: string;
  genre: string | null;
  mechanicCount: number;
  aestheticCount: number;
  opinionCount: number;
  size: number;
  updatedAt: number;
  mechanics: Array<{
    id: string;
    name: string;
    description: string | null;
    intends: string | null;
    intendedAffect: string | null;
  }>;
  aesthetics: Array<{
    id: string;
    name: string;
    description: string | null;
  }>;
}

export interface GameLearningSnapshot {
  layer: "games";
  workspaceId: string;
  generatedAt: number;
  totals: {
    games: number;
    mechanics: number;
    aesthetics: number;
    opinions: number;
  };
  games: GameLearningEntry[];
}

export type LearningLayerSnapshot =
  | KnowledgeLayerSnapshot
  | GameLearningSnapshot
  | (ReturnType<typeof buildTopicOpinionSnapshot> & { layer: "opinions" });

export interface LearningLayerOptions {
  limit?: number;
  detailLimit?: number;
}

export function buildLearningLayerSnapshot(
  db: Database.Database,
  workspaceId: string,
  layer: LearningLayer,
  options: LearningLayerOptions = {}
): LearningLayerSnapshot {
  if (layer === "knowledge") {
    return buildKnowledgeLayerSnapshot(db, workspaceId, options);
  }
  if (layer === "games") {
    return buildGameLearningSnapshot(db, workspaceId, options);
  }
  return {
    layer: "opinions",
    ...buildTopicOpinionSnapshot(db, workspaceId, {
      limit: options.limit,
      opinionsPerTopic: options.detailLimit,
    }),
  };
}

export function normalizeLearningLayer(value: string | undefined): LearningLayer {
  if (value === "knowledge" || value === "games" || value === "opinions") return value;
  return "knowledge";
}

function buildKnowledgeLayerSnapshot(
  db: Database.Database,
  workspaceId: string,
  options: LearningLayerOptions
): KnowledgeLayerSnapshot {
  const limit = clampLimit(options.limit ?? 80, 1, 300);
  const vocabulary = readAffectVocabulary(workspaceId);
  const usageStmt = db.prepare(
    `SELECT
       (SELECT COUNT(*) FROM mechanics WHERE workspace_id = ? AND intended_affect = ?) +
       (SELECT COUNT(*) FROM affects WHERE workspace_id = ? AND mood = ?) +
       (SELECT COUNT(*) FROM design_gaps WHERE workspace_id = ? AND (expected_affect = ? OR observed_affect = ?)) AS c`
  );
  const affects = vocabulary
    .map((affect) => {
      const usageCount = (
        usageStmt.get(
          workspaceId,
          affect.key,
          workspaceId,
          affect.key,
          workspaceId,
          affect.key,
          affect.key
        ) as { c: number }
      ).c;
      return {
        ...affect,
        usageCount,
        size: Math.max(1, usageCount),
      };
    })
    .sort((a, b) => b.size - a.size || a.key.localeCompare(b.key))
    .slice(0, limit);

  return {
    layer: "knowledge",
    workspaceId,
    generatedAt: Date.now(),
    totals: {
      affects: vocabulary.length,
      mechanics: count(db, "mechanics", workspaceId),
      aesthetics: count(db, "aesthetics", workspaceId),
      games: count(db, "games", workspaceId),
      gaps: count(db, "design_gaps", workspaceId),
      hypotheses: count(db, "hypotheses", workspaceId),
    },
    affects,
  };
}

function buildGameLearningSnapshot(
  db: Database.Database,
  workspaceId: string,
  options: LearningLayerOptions
): GameLearningSnapshot {
  const limit = clampLimit(options.limit ?? 50, 1, 200);
  const detailLimit = clampLimit(options.detailLimit ?? 8, 1, 50);
  const games = db
    .prepare(
      `SELECT id, title, genre, updated_at
         FROM games
        WHERE workspace_id = ?
        ORDER BY updated_at DESC
        LIMIT ?`
    )
    .all(workspaceId, limit) as Array<{
    id: string;
    title: string;
    genre: string | null;
    updated_at: number;
  }>;

  const mechanicCountStmt = db.prepare(
    "SELECT COUNT(*) AS c FROM mechanics WHERE workspace_id = ? AND game_id = ?"
  );
  const aestheticCountStmt = db.prepare(
    "SELECT COUNT(*) AS c FROM aesthetics WHERE workspace_id = ? AND game_id = ?"
  );
  const opinionCountStmt = db.prepare(
    `SELECT COUNT(*) AS c
       FROM hypotheses h
       JOIN design_gaps g ON g.id = h.design_gap_id AND g.workspace_id = h.workspace_id
      WHERE h.workspace_id = ? AND g.game_id = ?`
  );
  const mechanicsStmt = db.prepare(
    `SELECT id, name, description, intends, intended_affect
       FROM mechanics
      WHERE workspace_id = ? AND game_id = ?
      ORDER BY updated_at DESC
      LIMIT ?`
  );
  const aestheticsStmt = db.prepare(
    `SELECT id, name, description
       FROM aesthetics
      WHERE workspace_id = ? AND game_id = ?
      ORDER BY updated_at DESC
      LIMIT ?`
  );

  const entries = games
    .map((game) => {
      const mechanicCount = (mechanicCountStmt.get(workspaceId, game.id) as { c: number }).c;
      const aestheticCount = (aestheticCountStmt.get(workspaceId, game.id) as { c: number }).c;
      const opinionCount = (opinionCountStmt.get(workspaceId, game.id) as { c: number }).c;
      const mechanics = (
        mechanicsStmt.all(workspaceId, game.id, detailLimit) as Array<{
          id: string;
          name: string;
          description: string | null;
          intends: string | null;
          intended_affect: string | null;
        }>
      ).map((mechanic) => ({
        id: mechanic.id,
        name: mechanic.name,
        description: mechanic.description,
        intends: mechanic.intends,
        intendedAffect: mechanic.intended_affect,
      }));
      const aesthetics = aestheticsStmt.all(workspaceId, game.id, detailLimit) as Array<{
        id: string;
        name: string;
        description: string | null;
      }>;
      return {
        id: game.id,
        title: game.title,
        genre: game.genre,
        mechanicCount,
        aestheticCount,
        opinionCount,
        size: mechanicCount * 2 + aestheticCount + opinionCount * 3,
        updatedAt: game.updated_at,
        mechanics,
        aesthetics,
      };
    })
    .sort((a, b) => b.size - a.size || b.updatedAt - a.updatedAt);

  return {
    layer: "games",
    workspaceId,
    generatedAt: Date.now(),
    totals: {
      games: entries.length,
      mechanics: entries.reduce((sum, game) => sum + game.mechanicCount, 0),
      aesthetics: entries.reduce((sum, game) => sum + game.aestheticCount, 0),
      opinions: entries.reduce((sum, game) => sum + game.opinionCount, 0),
    },
    games: entries,
  };
}

interface VocabularyFile {
  affects?: Array<{
    key?: unknown;
    label_ja?: unknown;
    valence?: unknown;
    mda?: unknown;
    description?: unknown;
  }>;
}

function readAffectVocabulary(workspaceId: string): KnowledgeAffectEntry[] {
  const filePath = path.resolve("data/affects/vocabulary.json");
  let parsed: VocabularyFile = {};
  try {
    parsed = JSON.parse(fs.readFileSync(filePath, "utf8")) as VocabularyFile;
  } catch {
    return [];
  }
  if (!Array.isArray(parsed.affects)) return [];
  return parsed.affects
    .map((entry) => {
      const key = stringOrNull(entry.key);
      if (!key) return null;
      return {
        key,
        labelJa: stringOrNull(entry.label_ja),
        valence: stringOrNull(entry.valence),
        mda: stringOrNull(entry.mda),
        description: stringOrNull(entry.description),
        usageCount: 0,
        size: 1,
      };
    })
    .filter((entry): entry is KnowledgeAffectEntry => entry !== null);
}

function count(db: Database.Database, table: string, workspaceId: string): number {
  return (db.prepare(`SELECT COUNT(*) AS c FROM ${table} WHERE workspace_id = ?`).get(workspaceId) as { c: number }).c;
}

function stringOrNull(value: unknown): string | null {
  return typeof value === "string" && value.trim() ? value.trim() : null;
}

function clampLimit(value: number, min: number, max: number): number {
  if (!Number.isFinite(value)) return min;
  return Math.min(max, Math.max(min, Math.floor(value)));
}
