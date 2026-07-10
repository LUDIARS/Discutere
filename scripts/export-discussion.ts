/**
 * 議論を個別 md ファイルとしてディスクに書き出す CLI (議論 md エクスポート)。
 *
 *   npm run export:discussions                    # 全議論 (旧 + 新フロー) を ./data/export/discussions/ へ
 *   npm run export:discussions -- --out <dir>     # 出力先を指定
 *   npm run export:discussions -- --id <gapId>    # 1 件だけ ("flow:<sessionId>" or design_gap id)
 *   npm run export:discussions -- --limit 50
 *
 * Web UI の「md エクスポート」ボタン (GET /learning/conclusion/export) と同じレンダラを使う。
 */

import fs from "node:fs";
import path from "node:path";

import { getConfig } from "../src/config.js";
import { createCore } from "../src/core/index.js";
import { resolveActiveKgPath } from "../src/core/kg-registry.js";
import { openAttributionStore } from "../src/crawler/sources/attribution-store.js";
import {
  listConclusions,
  getConclusionDetail,
  type ConclusionDetail,
} from "../src/visualize/conclusions.js";
import { getFlowDb } from "../src/flow/db/connection.js";
import {
  listFlowConclusions,
  getFlowConclusionDetail,
  isFlowConclusionId,
  flowSessionIdFromGapId,
} from "../src/visualize/flow-conclusions.js";
import {
  renderConclusionMarkdown,
  safeSlug,
} from "../src/visualize/conclusion-markdown.js";
import { exportDiscussionToFundamentum } from "../src/fundamentum-export/index.js";

interface Args {
  out: string;
  id?: string;
  limit: number;
  fundamentum: boolean;
}

function parseArgs(argv: string[]): Args {
  const out: Args = { out: "./data/export/discussions", limit: 200, fundamentum: false };
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i] === "--out") out.out = argv[++i];
    else if (argv[i] === "--id") out.id = argv[++i];
    else if (argv[i] === "--limit") out.limit = Number(argv[++i]) || out.limit;
    else if (argv[i] === "--fundamentum") out.fundamentum = true;
  }
  return out;
}

function writeMd(dir: string, detail: ConclusionDetail): string {
  const md = renderConclusionMarkdown(detail);
  const file = path.join(dir, `${safeSlug(detail.title, detail.sessionId || "discussion")}.md`);
  fs.writeFileSync(file, md, "utf8");
  return file;
}

/** --fundamentum 指定時は md の代わりに Fundamentum へ export し、結果を文字列で返す。 */
async function exportOne(dir: string, detail: ConclusionDetail, useFundamentum: boolean): Promise<string> {
  if (useFundamentum) {
    const result = await exportDiscussionToFundamentum(detail);
    return `fundamentum: ${result.discussionId} -> ${result.contentId} (catalog v${result.catalogVersion})`;
  }
  return path.relative(process.cwd(), writeMd(dir, detail));
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));
  const config = getConfig();
  fs.mkdirSync(path.resolve(args.out), { recursive: true });

  const core = createCore(resolveActiveKgPath(config));
  const attribution = openAttributionStore();
  const flowDb = getFlowDb();
  let written = 0;
  try {
    // 1 件指定
    if (args.id) {
      const detail = isFlowConclusionId(args.id)
        ? getFlowConclusionDetail(flowDb, flowSessionIdFromGapId(args.id))
        : getConclusionDetail(core, config.workspace, args.id, attribution);
      if (!detail) {
        console.error(`結論が見つかりません: ${args.id}`);
        process.exit(1);
      }
      const out = await exportOne(args.out, detail, args.fundamentum);
      console.log(`書き出し: ${out}`);
      return;
    }

    // 全件 (旧 design_gap + 新フロー)
    for (const s of listConclusions(core, config.workspace, args.limit)) {
      const detail = getConclusionDetail(core, config.workspace, s.gapId, attribution);
      if (detail) {
        await exportOne(args.out, detail, args.fundamentum);
        written += 1;
      }
    }
    for (const s of listFlowConclusions(flowDb, args.limit)) {
      const detail = getFlowConclusionDetail(flowDb, s.sessionId);
      if (detail) {
        await exportOne(args.out, detail, args.fundamentum);
        written += 1;
      }
    }
    console.log(
      args.fundamentum
        ? `${written} 件を Fundamentum (${config.fundamentum.dataDir}) へ export しました`
        : `${written} 件を ${path.resolve(args.out)} に書き出しました`
    );
  } finally {
    attribution.close();
    core.close();
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
