/**
 * ② 分析対象の取り込み。
 *
 * slug が data/games/ に存在すれば KG md frontmatter から取得。
 * 存在しなければ spec テキストから LLM 類推 (Phase 0 は LLM 類推ルートをモック可)。
 *
 * spec/plan/RESTRUCTURE.md §2-②。
 */

import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { randomUUID } from "node:crypto";
import type { GameIntake, MechanicMid, Step2Output } from "./types.js";

// ── frontmatter パーサ ────────────────────────────────────────────────────────

interface RawMechanic {
  name?: string;
  description?: string;
  intends?: string;
  intended_affect?: string;
  play_context?: string;
}

interface RawFrontmatter {
  id?: string;
  title?: string;
  genre?: string;
  workspace_id?: string;
  mechanics?: RawMechanic[];
  sources?: Array<{ url?: string }>;
}

/** YAML frontmatter をシンプルに構造化する。完全 YAML パーサは不使用 (依存追加なし)。
 *  CRLF / LF 両対応のため行単位パース。 */
function parseFrontmatter(mdContent: string): RawFrontmatter {
  // CRLF 正規化
  const content = mdContent.replace(/\r\n/g, "\n");
  const match = /^---\n([\s\S]*?)\n---/.exec(content);
  if (!match) return {};

  const lines = match[1].split("\n");

  let id: string | undefined;
  let title: string | undefined;
  let genre: string | undefined;
  const mechanics: RawMechanic[] = [];

  let inMechanics = false;
  let current: Partial<RawMechanic> | null = null;

  function flush() {
    if (current?.name) mechanics.push(current as RawMechanic);
    current = null;
  }

  for (const line of lines) {
    const scalar = (v: string) => v.replace(/^["']|["']$/g, "").trim();

    // トップレベルキー判定
    const topKey = /^(\w[\w_]*):\s*(.*)/.exec(line);
    if (topKey && !line.startsWith(" ") && !line.startsWith("\t")) {
      const key = topKey[1];
      const val = scalar(topKey[2]);
      inMechanics = key === "mechanics";
      if (!inMechanics) flush();
      if (key === "id") id = val;
      else if (key === "title") title = val;
      else if (key === "genre") genre = val;
      continue;
    }

    if (!inMechanics) continue;

    // mechanics 内の list entry
    const listStart = /^\s{2}-\s+name:\s*(.+)/.exec(line);
    if (listStart) {
      flush();
      current = { name: scalar(listStart[1]) };
      continue;
    }
    if (current) {
      const kv = /^\s{4}(\w[\w_]*):\s*(.+)/.exec(line);
      if (kv) {
        const k = kv[1] as keyof RawMechanic;
        (current as Record<string, string>)[k] = scalar(kv[2]);
      }
    }
  }
  flush();

  return { id, title, genre, mechanics };
}

// ── sentiment sidecar 読み込み ────────────────────────────────────────────────

async function loadSentimentSidecar(gamesDir: string, slug: string): Promise<Record<string, unknown> | undefined> {
  const path = join(gamesDir, `${slug}.sentiment.json`);
  try {
    const raw = await readFile(path, "utf-8");
    return JSON.parse(raw) as Record<string, unknown>;
  } catch {
    return undefined;
  }
}

// ── KG 取得ルート ─────────────────────────────────────────────────────────────

async function loadFromKg(gamesDir: string, slug: string): Promise<GameIntake | null> {
  const mdPath = join(gamesDir, `${slug}.md`);
  const mdRaw = await readFile(mdPath, "utf-8").catch(() => null);
  if (!mdRaw) return null;

  const fm = parseFrontmatter(mdRaw);
  const mechanics: MechanicMid[] = (fm.mechanics ?? []).map((m) => ({
    id: randomUUID(),
    name: m.name ?? "unnamed",
    description: m.description,
    intends: m.intends,
    intendedAffect: m.intended_affect,
  }));

  const sentimentSidecar = await loadSentimentSidecar(gamesDir, slug);

  return {
    slug,
    title: fm.title ?? slug,
    genre: fm.genre,
    source: "kg",
    mechanics,
    sentimentSidecar,
  };
}

// ── spec 類推ルート (Phase 0 はモックテキストを受ける) ────────────────────────

interface SpecIntakeOptions {
  title: string;
  genre?: string;
  specText: string;
}

/** Phase 0: spec テキストから mechanics を抽出 (LLM 注入可) */
export async function intakeFromSpec(
  opts: SpecIntakeOptions,
  extractMechanics: (specText: string) => Promise<MechanicMid[]>,
): Promise<GameIntake> {
  const slug = opts.title.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
  const mechanics = await extractMechanics(opts.specText);
  return {
    slug,
    title: opts.title,
    genre: opts.genre,
    source: "spec",
    mechanics,
  };
}

// ── 公開 API ──────────────────────────────────────────────────────────────────

export async function intakeGame(gamesDir: string, slug: string): Promise<Step2Output> {
  const kg = await loadFromKg(gamesDir, slug);
  if (kg) return kg;

  // KG に無い場合: Phase 0 では spec ルートの代わりにエラーで止める
  // (spec 類推は intakeFromSpec を直接呼ぶ)
  throw new Error(`Game not found in KG: ${slug}. Use intakeFromSpec for spec-only input.`);
}

/** 利用可能なゲーム slug 一覧 (data/games/*.md) */
export async function listAvailableSlugs(gamesDir: string): Promise<string[]> {
  const { readdir } = await import("node:fs/promises");
  const entries = await readdir(gamesDir).catch(() => []);
  return entries
    .filter((e) => e.endsWith(".md"))
    .map((e) => e.replace(/\.md$/, ""));
}
