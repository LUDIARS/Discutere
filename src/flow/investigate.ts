/**
 * テーマ調査 (discussion.md step 2)。
 *
 * メカニクス + 感情データを収集し、0 件の場合は YouTube で補完する。
 * canCollectExternal が false (機密/内部) な場合は外部取得しない。
 */

import fs from "node:fs";
import path from "node:path";
import type { FlowTag } from "./tags.js";
import { canCollectExternal } from "./tags.js";

export interface MechanicSummary {
  name: string;
  description: string;
  intended_affect?: string;
  intended_valence?: string;
}

export interface InvestigateResult {
  mechanics: MechanicSummary[];
  sentimentCount: number;
  /** YouTube 補完が実行されたか */
  youtubeUsed: boolean;
  /** YouTube コメント本文 (感情付与前) */
  youtubeComments: string[];
}

export interface YoutubeSearchFn {
  (query: string, maxComments: number): Promise<string[]>;
}

export interface InvestigateArgs {
  theme: string;
  tags: readonly FlowTag[];
  /** `data/games/` ディレクトリパス (既定 `./data/games`) */
  gamesDir?: string;
  /** YouTube 検索実装 (注入式、テストでは mock を渡す) */
  youtubeSearch?: YoutubeSearchFn;
  /** YouTube 最大コメント数 */
  youtubeMaxComments?: number;
  /** 外部取得の警告を出す関数 */
  warn?: (msg: string) => void;
  /** 既存の感情データ件数を返す関数 (注入式) */
  getSentimentCount?: (theme: string) => number;
}

/**
 * `data/games/` ディレクトリから全メカニクスをロードする (gray-matter を使わず簡易パース)。
 * テーマに関連しそうなゲームのメカニクスを抽出する。
 */
function loadMechanics(gamesDir: string, theme: string): MechanicSummary[] {
  if (!fs.existsSync(gamesDir)) return [];

  const files = fs
    .readdirSync(gamesDir)
    .filter((f) => f.endsWith(".md") && !f.includes(".sentiment"));

  const results: MechanicSummary[] = [];
  const lowerTheme = theme.toLowerCase();

  for (const file of files) {
    const content = fs.readFileSync(path.join(gamesDir, file), "utf-8");
    // YAML フロントマターから mechanics を抽出 (簡易パーサー)
    const match = content.match(/^mechanics:\s*\n([\s\S]*?)(?=^[a-z]|\Z)/m);
    if (!match) continue;

    const mechanicsSection = match[1];
    // - name: ... を解析
    const items = mechanicsSection.split(/^\s*-\s+name:/m).slice(1);

    for (const item of items) {
      const nameLine = item.split("\n")[0]?.trim().replace(/^["']|["']$/g, "") ?? "";
      const descMatch = item.match(/description:\s*["']?([\s\S]*?)["']?\s*\n/);
      const affectMatch = item.match(/intended_affect:\s*["']?(.*?)["']?\s*\n/);
      const valenceMatch = item.match(/intended_valence:\s*["']?(.*?)["']?\s*\n/);

      const desc = descMatch?.[1]?.trim() ?? "";
      const name = nameLine;

      if (!name) continue;

      // テーマとの関連チェック (ゲームタイトル or メカニクス名の部分一致)
      const relevant =
        lowerTheme.split(/\s+/).some((word) => word.length >= 2 && (name + desc).toLowerCase().includes(word)) ||
        file.toLowerCase().includes(lowerTheme.replace(/\s+/g, "-").slice(0, 8));

      if (relevant || results.length < 3) {
        results.push({
          name,
          description: desc,
          intended_affect: affectMatch?.[1]?.trim(),
          intended_valence: valenceMatch?.[1]?.trim(),
        });
      }
    }
  }

  return results.slice(0, 10);
}

/** テーマに関連する sentiment.json を探し件数を返す (デフォルト実装)。 */
function defaultGetSentimentCount(gamesDir: string, theme: string): number {
  if (!fs.existsSync(gamesDir)) return 0;
  const files = fs.readdirSync(gamesDir).filter((f) => f.endsWith(".sentiment.json"));
  const lowerTheme = theme.toLowerCase();
  let count = 0;
  for (const f of files) {
    if (lowerTheme.split(/\s+/).some((w) => w.length >= 2 && f.toLowerCase().includes(w))) {
      try {
        const data = JSON.parse(fs.readFileSync(path.join(gamesDir, f), "utf-8")) as unknown;
        if (Array.isArray(data)) count += data.length;
        else if (data && typeof data === "object" && "comments" in data) {
          count += (data as { comments: unknown[] }).comments.length;
        }
      } catch {
        // パース失敗は無視
      }
    }
  }
  return count;
}

export async function investigateTheme(args: InvestigateArgs): Promise<InvestigateResult> {
  const {
    theme,
    tags,
    gamesDir = "./data/games",
    youtubeSearch,
    youtubeMaxComments = 200,
    warn = (msg) => console.warn(`[investigate] ${msg}`),
    getSentimentCount,
  } = args;

  const mechanics = loadMechanics(gamesDir, theme);

  const sentimentCount = getSentimentCount
    ? getSentimentCount(theme)
    : defaultGetSentimentCount(gamesDir, theme);

  let youtubeUsed = false;
  let youtubeComments: string[] = [];

  // YouTube 補完条件: 感情データ 0 件 + 外部収集 OK + youtubeSearch 実装あり
  if (sentimentCount === 0 && canCollectExternal(tags) && youtubeSearch) {
    warn(
      `感情データが見つかりません (テーマ: "${theme}")。YouTube でコメントを検索します。` +
        ` (最大 ${youtubeMaxComments} 件)`
    );
    try {
      youtubeComments = await youtubeSearch(theme, youtubeMaxComments);
      youtubeUsed = true;
    } catch (err) {
      warn(`YouTube 検索に失敗しました: ${(err as Error).message}`);
    }
  }

  return { mechanics, sentimentCount, youtubeUsed, youtubeComments };
}
