/**
 * Affect 統制語彙シーダ (Phase 0)
 *
 *   npm run seed:affects                  # data/affects/vocabulary.json → affects テーブル (workspace=knowledge)
 *   npm run seed:affects -- --emit-negatives  # 語彙の valence==="negative" から
 *                                              # src/core/bridge/gap/affect-negatives.ts を再生成
 *
 * canonical 語彙を id=`affect:<key>` で upsert する (idempotent)。
 * vocabulary_status="canonical" / valence を直接セット (affect repo は valence 未対応のため raw insert)。
 * provisional 語 (LLM 調査済) は本シーダの対象外 (別 id・status="provisional" で投入される)。
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { createCore } from "../src/core/index.js";

interface VocabEntry { key: string; label_ja: string; valence: "positive" | "negative" | "ambivalent"; description?: string }

const HERE = path.dirname(fileURLToPath(import.meta.url));
const VOCAB_PATH = path.resolve(HERE, "../data/affects/vocabulary.json");
const NEGATIVES_PATH = path.resolve(HERE, "../src/core/bridge/gap/affect-negatives.ts");

function loadVocab(): { workspace_id: string; affects: VocabEntry[] } {
  return JSON.parse(fs.readFileSync(VOCAB_PATH, "utf8").replace(/^﻿/, ""));
}

function emitNegatives(): void {
  const { affects } = loadVocab();
  const negs = affects.filter((a) => a.valence === "negative").map((a) => a.key);
  const body = `// 自動生成 (npm run seed:affects -- --emit-negatives)。source of truth: data/affects/vocabulary.json (valence==="negative")。
// matcher を pure に保つため runtime で json を読まず生成値を埋め込む。手で編集しない。
export const NEGATIVE_AFFECTS: ReadonlySet<string> = new Set([
${negs.map((k) => `  ${JSON.stringify(k)},`).join("\n")}
]);
`;
  fs.writeFileSync(NEGATIVES_PATH, body);
  console.log(`emitted ${negs.length} negatives -> ${path.relative(process.cwd(), NEGATIVES_PATH)}`);
}

function seed(): void {
  const { workspace_id, affects } = loadVocab();
  const ws = workspace_id || "knowledge";
  const core = createCore();
  const now = Date.now();
  const stmt = core.client.raw.prepare(
    `INSERT INTO affects (id, workspace_id, session_id, subject_id, mood, score, created_at, updated_at, valence, vocabulary_status)
     VALUES (@id, @ws, NULL, NULL, @mood, NULL, @now, @now, @valence, @status)
     ON CONFLICT(id) DO UPDATE SET mood=excluded.mood, valence=excluded.valence,
       vocabulary_status=excluded.vocabulary_status, updated_at=excluded.updated_at`
  );
  let n = 0;
  for (const a of affects) {
    stmt.run({ id: `affect:${a.key}`, ws, mood: a.key, valence: a.valence, now, status: (a as any).status ?? "canonical" });
    n += 1;
  }
  core.close();
  console.log(`seeded ${n} canonical affects into workspace="${ws}"`);
}

function main(): void {
  if (process.argv.includes("--emit-negatives")) return emitNegatives();
  seed();
}

main();
