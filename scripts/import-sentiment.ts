/**
 * SENTIMENT サイドカーを Core DB (SQLite) に配線する。
 *
 * data/games/*.sentiment.json を読み、
 *   affects[]   → affects テーブル (idempotent: ON CONFLICT(id) DO UPDATE)
 *   clusters[]  → embeddings テーブル nodeType="discussion_cluster"
 *   game_vector → embeddings テーブル nodeType="game_sentiment"
 *
 * spec/crawler/SENTIMENT.md §TODO: "DB import 配線"
 *
 * Usage:
 *   npx tsx scripts/import-sentiment.ts                # data/games/*.sentiment.json 全件
 *   npx tsx scripts/import-sentiment.ts <slug>         # 指定 slug のみ
 *   npx tsx scripts/import-sentiment.ts --dry-run      # 書き込まず件数だけ表示
 */

import fs from "node:fs";
import path from "node:path";
import { glob } from "node:fs/promises";
import { fileURLToPath } from "node:url";

import { createCore } from "../src/core/index.js";
import { getConfig } from "../src/config.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const GAMES_DIR = path.resolve(HERE, "../data/games");

interface SentimentAffect {
  subject: string;
  mood: string;
  valence: string;
  score: number;
}

interface SentimentCluster {
  id: string;
  topic_aspect: string;
  size: number;
  sentiment: string;
  score: number;
  vector: number[];
}

interface SentimentSidecar {
  game: string;
  slug: string;
  overall?: { valence?: string; score?: number };
  game_vector?: number[];
  affects?: SentimentAffect[];
  clusters?: SentimentCluster[];
}

function affectId(slug: string, subject: string): string {
  return `sentiment:${slug}:${subject}`;
}

async function importSidecar(
  sidecarPath: string,
  core: ReturnType<typeof createCore>,
  workspaceId: string,
  dryRun: boolean,
): Promise<{ affectsUpserted: number; embeddingsUpserted: number }> {
  const raw = fs.readFileSync(sidecarPath, "utf-8").replace(/^﻿/, "");
  const sidecar = JSON.parse(raw) as SentimentSidecar;
  const { slug } = sidecar;

  let affectsUpserted = 0;
  let embeddingsUpserted = 0;

  // affects[]
  const affects = sidecar.affects ?? [];
  if (affects.length > 0 && !dryRun) {
    const stmt = core.client.raw.prepare(
      `INSERT INTO affects (id, workspace_id, session_id, subject_id, mood, score, valence, created_at, updated_at)
       VALUES (@id, @workspaceId, NULL, @subjectId, @mood, @score, @valence, @now, @now)
       ON CONFLICT(id) DO UPDATE SET
         mood=excluded.mood, score=excluded.score, valence=excluded.valence, updated_at=excluded.updated_at`,
    );
    const now = Date.now();
    for (const a of affects) {
      stmt.run({
        id: affectId(slug, a.subject),
        workspaceId,
        subjectId: `${slug}:${a.subject}`,
        mood: a.mood,
        score: a.score,
        valence: a.valence,
        now,
      });
      affectsUpserted++;
    }
  } else {
    affectsUpserted = affects.length;
  }

  // clusters[] → embeddings
  const clusters = sidecar.clusters ?? [];
  if (!dryRun) {
    for (const c of clusters) {
      if (!c.vector || c.vector.length === 0) continue;
      await core.vectors.registerEmbedding({
        workspaceId,
        nodeType: "discussion_cluster",
        nodeId: `${slug}:${c.id}`,
        vector: c.vector,
      });
      embeddingsUpserted++;
    }
  } else {
    embeddingsUpserted += clusters.filter((c) => c.vector?.length > 0).length;
  }

  // game_vector → embeddings
  if (sidecar.game_vector && sidecar.game_vector.length > 0) {
    if (!dryRun) {
      await core.vectors.registerEmbedding({
        workspaceId,
        nodeType: "game_sentiment",
        nodeId: slug,
        vector: sidecar.game_vector,
      });
    }
    embeddingsUpserted++;
  }

  return { affectsUpserted, embeddingsUpserted };
}

async function main(): Promise<void> {
  const args = process.argv.slice(2);
  const dryRun = args.includes("--dry-run");
  const filterSlug = args.find((a) => !a.startsWith("--"));

  const files: string[] = [];
  for await (const f of glob(path.join(GAMES_DIR, "*.sentiment.json"))) {
    files.push(f as string);
  }

  if (files.length === 0) {
    console.log("No *.sentiment.json files found in data/games/");
    return;
  }

  const config = getConfig();
  const kgPath = resolveActiveKgPath(config);
  const core = createCore(kgPath);

  let totalAffects = 0;
  let totalEmbeddings = 0;
  const workspaceId = config.workspace || "knowledge";

  try {
    for (const f of files) {
      const slug = path.basename(f, ".sentiment.json");
      if (filterSlug && slug !== filterSlug) continue;

      const { affectsUpserted, embeddingsUpserted } = await importSidecar(f, core, workspaceId, dryRun);
      totalAffects += affectsUpserted;
      totalEmbeddings += embeddingsUpserted;
      console.log(
        `  ${dryRun ? "[dry]" : "     "} ${slug}: affects=${affectsUpserted} embeddings=${embeddingsUpserted}`,
      );
    }
  } finally {
    core.close();
  }

  console.log(
    `\n${dryRun ? "Dry-run" : "Imported"}: affects=${totalAffects} embeddings=${totalEmbeddings} workspace="${workspaceId}"`,
  );
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
