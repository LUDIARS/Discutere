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
  // ---- 運営の想定感情 (Gap率分析用の構造化定義、任意) ----
  /** 運営が狙う感情極性。Gap率は観測 valence がこれと不一致な割合で測る */
  intended_valence?: "positive" | "negative" | "neutral";
  /** この mechanic が駆動するはずの aspect (fun/difficulty/price_value 等、lexicon の 8 種) */
  intended_aspects?: string[];
  /** 運営が狙う Plutchik 感情 (joy/anticipation 等、任意) */
  intended_emotions?: string[];
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
