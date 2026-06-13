/**
 * data/games/<slug>.md のメカニクス frontmatter 読み書き。
 *
 * 学習フロー (learning.ts) が収集したメカニクスを議論フローの調査 (investigate.ts の
 * loadMechanics) が読める形 — data/games/<slug>.md の frontmatter `mechanics:` — に
 * 永続する。gray-matter は既存依存 (investigate.ts と同じ)。
 */

import fs from "node:fs";
import path from "node:path";
import matter from "gray-matter";
import type { MechanicSummary } from "./investigate.js";

/** 1 メカニクス frontmatter エントリ (loadMechanics が読むキー)。 */
export interface GameMechanicEntry {
  name: string;
  description?: string;
  intended_affect?: string;
  intended_valence?: string;
  intended_aspects?: string[];
  intended_emotions?: string[];
}

/**
 * data/games/<slug>.md に mechanics を upsert する (name 一致でマージ)。
 * ファイルが無ければ最小 frontmatter (id/title/mechanics) で新規作成する。
 * 既存の他フィールド (sources, genre, 本文) は保持する。
 *
 * @returns 追加 + 更新されたメカニクス件数
 */
export function upsertGameMechanics(args: {
  gamesDir: string;
  slug: string;
  title: string;
  mechanics: GameMechanicEntry[];
}): number {
  const { gamesDir, slug, title, mechanics } = args;
  if (mechanics.length === 0) return 0;

  if (!fs.existsSync(gamesDir)) fs.mkdirSync(gamesDir, { recursive: true });
  const filePath = path.join(gamesDir, `${slug}.md`);

  let data: Record<string, unknown> = {};
  let body = "";
  if (fs.existsSync(filePath)) {
    const parsed = matter(fs.readFileSync(filePath, "utf-8"));
    data = parsed.data ?? {};
    body = parsed.content ?? "";
  } else {
    data = { id: slug, title };
    body = `# ${title}\n\n学習フローで収集したメカニクス。\n`;
  }

  const existing = Array.isArray(data.mechanics) ? (data.mechanics as GameMechanicEntry[]) : [];
  const byName = new Map(existing.map((m) => [m.name, m]));

  for (const m of mechanics) {
    if (!m.name) continue;
    // 未定義キーを混ぜないよう定義済みプロパティのみで構築
    const entry: GameMechanicEntry = { name: m.name };
    if (m.description !== undefined) entry.description = m.description;
    if (m.intended_affect !== undefined) entry.intended_affect = m.intended_affect;
    if (m.intended_valence !== undefined) entry.intended_valence = m.intended_valence;
    if (m.intended_aspects !== undefined) entry.intended_aspects = m.intended_aspects;
    if (m.intended_emotions !== undefined) entry.intended_emotions = m.intended_emotions;
    byName.set(m.name, { ...byName.get(m.name), ...entry });
  }

  data.mechanics = [...byName.values()];
  fs.writeFileSync(filePath, matter.stringify(body, data), "utf-8");
  return mechanics.filter((m) => m.name).length;
}

/** MechanicSummary[] (investigate の型) を frontmatter エントリへ変換する補助。 */
export function mechanicSummaryToEntry(m: MechanicSummary): GameMechanicEntry {
  return {
    name: m.name,
    description: m.description,
    intended_affect: m.intended_affect,
    intended_valence: m.intended_valence,
    intended_aspects: m.intended_aspects,
    intended_emotions: m.intended_emotions,
  };
}
