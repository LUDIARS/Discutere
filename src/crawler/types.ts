/**
 * Crawler KG types (Phase 0)
 *
 * data/games/<slug>.md の frontmatter ↔ TypeScript オブジェクトの正本定義。
 * Discatier Core (Game / Mechanic / Aesthetic) と 1:1 対応する。
 */

export interface SourceRef {
  url: string;
  title?: string;
  fetched_at?: string; // ISO date (YYYY-MM-DD or full)
  attribution?: string; // ライセンス表記 / 帰属
  excerpt_policy?: "summary-only" | "fair-use-quote" | "forbidden";
}

export interface MechanicEntry {
  name: string;
  description?: string;
  intends?: string; // designer intent (Discatier Mechanic.intends)
  intended_affect?: string; // Phase 4 affect (Discatier Mechanic.intended_affect)
}

export interface AestheticEntry {
  name: string;
  description?: string;
}

export interface GameKG {
  /** slug (kebab-case)。 Game.id の dedup key として使う */
  id: string;
  title: string;
  /** 「大まかなジャンル」 (Phase 0 は string、 Phase 2 で Genre ノード化) */
  genre?: string;
  workspace_id: string;
  sources: SourceRef[];
  mechanics: MechanicEntry[];
  aesthetics: AestheticEntry[];
  /** frontmatter の後ろにある人間用本文 (DB には入れない) */
  body?: string;
}

export const DEFAULT_WORKSPACE = "knowledge" as const;
