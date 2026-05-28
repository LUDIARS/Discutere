/**
 * Crawler runner (Phase 0 = interface のみ、 実装は Phase 1)
 *
 * Phase 1 では Claude API (WebSearch tool use) で
 *   gameName → 攻略サイト巡回 → 構造化抽出 → md (frontmatter + 本文)
 * を返す実装を入れる。
 * 制約は spec/crawler/DESIGN.md「制約」セクションを参照。
 */

import type { GameKG } from "./types.js";

export interface RunOptions {
  /** 巡回する最大ソース数 (Phase 1 で rate limit と合わせて使う) */
  maxSources?: number;
  /** 引用ポリシー (DESIGN.md「制約 / 引用」 参照、 default = summary-only) */
  excerptPolicy?: "summary-only" | "fair-use-quote" | "forbidden";
}

export interface RunResult {
  /** stringifyGameKG(kg) と同じ markdown */
  markdown: string;
  /** KG オブジェクト (importer に直接渡せる) */
  kg: GameKG;
  /** 実行メタ */
  meta: {
    gameName: string;
    ranAt: number;
    crawlerVersion: string;
    sourcesUsed: string[]; // canonical URL list
  };
}

export interface CrawlerRunner {
  run(gameName: string, options?: RunOptions): Promise<RunResult>;
}

/**
 * Phase 0 では実装なし。 ユーザは md を手書きするか、 別 Claude セッションで
 * 生成して data/games/<slug>.md に置く運用。
 */
export function createNoopRunner(): CrawlerRunner {
  return {
    async run() {
      throw new Error(
        "CrawlerRunner is not implemented in Phase 0. Author data/games/<slug>.md manually, then use `npx tsx scripts/crawl.ts import <path>`."
      );
    },
  };
}
