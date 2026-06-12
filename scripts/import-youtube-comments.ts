/**
 * YouTube コメント一括インポート — task #128 モンスト YouTube 外部音声取り込み。
 *
 * 外部の YouTube コメント JSONL (crawler 出力形式) を Di KG に外部発話として登録する。
 * 既存の importExternalUtterances() pipeline を使うため attribution / ingested dedup も機能する。
 *
 * 使い方:
 *   npx tsx scripts/import-youtube-comments.ts --input <path>.jsonl --game monster-strike
 *   npx tsx scripts/import-youtube-comments.ts --input <path>.jsonl --game monster-strike --dry-run
 *   npx tsx scripts/import-youtube-comments.ts --input <path>.jsonl --game monster-strike --limit 1000
 *
 * 入力 JSONL の各行は YouTube crawler 形式:
 *   { commentId, videoId, videoTitle, videoUrl, channelTitle, category,
 *     author, authorChannelId, text, likeCount, publishedAt, updatedAt, replyCount }
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createCore } from "../src/core/index.js";
import { getConfig } from "../src/config.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { importExternalUtterances } from "../src/crawler/sources/importer.js";
import { openAttributionStore, defaultAttributionPath } from "../src/crawler/sources/attribution-store.js";
import { openIngestedStore, DEFAULT_INGESTED_PATH } from "../src/crawler/sources/ingested-store.js";
import type { ExternalUtterance } from "../src/crawler/sources/types.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..");

interface YtComment {
  commentId: string;
  videoId: string;
  videoTitle: string;
  videoUrl: string;
  channelTitle: string;
  category?: string;
  author?: string;
  authorChannelId?: string;
  text: string;
  likeCount?: number;
  publishedAt: string;
  replyCount?: number;
}

function parseArgs(argv: string[]): { input: string; game: string; limit: number; dryRun: boolean; workspaceId: string } {
  const get = (name: string) => {
    const i = argv.indexOf(`--${name}`);
    return i >= 0 ? argv[i + 1] : undefined;
  };
  const input = get("input");
  if (!input) {
    console.error("--input <jsonl path> が必要です");
    process.exit(1);
  }
  return {
    input: path.resolve(input),
    game: get("game") ?? "monster-strike",
    limit: Number(get("limit") ?? 0),
    dryRun: argv.includes("--dry-run"),
    workspaceId: get("workspace") ?? "knowledge",
  };
}

function loadComments(jsonlPath: string, limit: number): YtComment[] {
  const text = fs.readFileSync(jsonlPath, "utf-8");
  const lines = text.split(/\r?\n/).filter((l) => l.trim());
  const items: YtComment[] = [];
  for (const line of lines) {
    try {
      items.push(JSON.parse(line) as YtComment);
    } catch {
      // malformed line — skip
    }
    if (limit > 0 && items.length >= limit) break;
  }
  return items;
}

/** authorChannelId を authorId に正規化 (未知なら author ハンドルを fallback)。 */
function resolveAuthorId(c: YtComment): string {
  return c.authorChannelId ?? c.author ?? `yt-anon-${c.commentId}`;
}

function toExternalUtterance(c: YtComment, gameSlug: string): ExternalUtterance {
  return {
    source: "youtube",
    nativeId: c.commentId,
    gameSlug,
    // threadKey = 動画単位 → session_id = ext:youtube:<videoId>
    threadKey: c.videoId,
    content: c.text,
    lang: "ja",
    postedAt: new Date(c.publishedAt).getTime(),
    authorId: resolveAuthorId(c),
    authorName: c.author,
    // likeCount > 0 は支持シグナル (YouTube に dislike 公開 API なし)
    signal: c.likeCount && c.likeCount > 0
      ? { votedUp: true, upvotes: c.likeCount }
      : undefined,
    sourceUrl: c.videoUrl,
  };
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));

  if (!fs.existsSync(args.input)) {
    console.error(`入力ファイルが見つかりません: ${args.input}`);
    process.exit(1);
  }

  const comments = loadComments(args.input, args.limit);
  console.log(`読込: ${comments.length} コメント (${args.input})`);

  if (args.dryRun) {
    // dry-run: 件数確認と先頭 3 件のプレビュー
    const items = comments.slice(0, 3).map((c) => toExternalUtterance(c, args.game));
    console.log("\n(dry-run) サンプル:");
    for (const item of items) {
      console.log(`  nativeId=${item.nativeId} threadKey=${item.threadKey}`);
      console.log(`    content="${item.content.slice(0, 60)}"`);
      console.log(`    sourceUrl=${item.sourceUrl}`);
    }
    console.log("\n(dry-run) DB 書き込みは行いません");
    return;
  }

  const config = getConfig(path.join(ROOT, "discutere.config.json"));
  const sqlitePath = resolveActiveKgPath(config) ?? path.join(ROOT, "data/discatier.kuzu");
  // Core は SQLite のみ (Kuzu グラフは今回スキップ — 後で kuzu:rebuild で補完可)
  const core = createCore(sqlitePath);
  const attribution = openAttributionStore(defaultAttributionPath());
  const ingested = openIngestedStore(DEFAULT_INGESTED_PATH);

  try {
    const items = comments.map((c) => toExternalUtterance(c, args.game));
    const result = importExternalUtterances(core, items, {
      workspaceId: args.workspaceId,
      ingested,
      attribution,
      sessionTitle: (item) => {
        // videoId に対応するタイトルをコメントから取る
        const orig = comments.find((c) => c.videoId === item.threadKey);
        return orig ? orig.videoTitle.slice(0, 100) : `youtube:${item.threadKey}`;
      },
    });

    console.log("\nインポート結果:");
    console.log(`  sessions: ${result.sessions} (新規)`);
    console.log(`  utterances: ${result.utterances} (追加)`);
    console.log(`  reactions: ${result.reactions} (いいね)`);
    console.log(`  skipped: ${result.skipped} (dedup)`);
  } finally {
    attribution.close();
    ingested.close();
    core.close();
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
