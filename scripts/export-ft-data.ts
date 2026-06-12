/**
 * FT 教師データエクスポート — spec/feature/gemma-ft.md (task #138)。
 *
 * data/worker-turns/*.json (turn) と対応する *.reply.json をスキャンし、
 * OpenAI chat JSONL 形式 (system/user/assistant) に変換して出力する。
 *
 * 出力先: data/ft-export/debate-YYYY-MM-DD.jsonl
 *
 * 使い方:
 *   npx tsx scripts/export-ft-data.ts               # 全件エクスポート
 *   npx tsx scripts/export-ft-data.ts --dry-run     # 件数確認のみ
 *   npx tsx scripts/export-ft-data.ts --out path    # 出力先指定
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const TURNS_DIR = path.resolve(HERE, "../data/worker-turns");
const WORKER_HOME_REPLIES = path.resolve(HERE, "../worker-home/replies");
const FT_EXPORT_DIR = path.resolve(HERE, "../data/ft-export");

interface TurnJson {
  reqId: string;
  workerId: string;
  system: string;
  prompt: string;
}

interface ReplyJson {
  reqId: string;
  workerId: string;
  text: string;
}

interface ChatSample {
  messages: Array<{ role: "system" | "user" | "assistant"; content: string }>;
  metadata: { reqId: string; workerId: string };
}

function loadJson<T>(filePath: string): T | null {
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf-8")) as T;
  } catch {
    return null;
  }
}

/** reqId に対応する reply を探す。turns dir → worker-home/replies の順で探す。 */
function findReply(reqId: string): ReplyJson | null {
  const inTurns = loadJson<ReplyJson>(path.join(TURNS_DIR, `${reqId}.reply.json`));
  if (inTurns) return inTurns;
  const inWorkerHome = loadJson<ReplyJson>(path.join(WORKER_HOME_REPLIES, `${reqId}.reply.json`));
  return inWorkerHome;
}

function buildSample(turn: TurnJson, reply: ReplyJson): ChatSample | null {
  const text = (reply.text ?? "").trim();
  if (!text) return null;

  // JSON-only response (e.g. {action:"skip"}) は学習対象から除外する
  if (text.startsWith("{") && text.includes('"action"')) {
    try {
      const parsed = JSON.parse(text);
      if (parsed.action === "skip") return null;
    } catch { /* non-JSON, keep */ }
  }

  return {
    messages: [
      { role: "system", content: turn.system.trim() },
      { role: "user", content: turn.prompt.trim() },
      { role: "assistant", content: text },
    ],
    metadata: { reqId: turn.reqId, workerId: turn.workerId },
  };
}

function todayStr(): string {
  const d = new Date();
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

function main(): void {
  const args = process.argv.slice(2);
  const dryRun = args.includes("--dry-run");
  const outIdx = args.indexOf("--out");
  const outFile = outIdx >= 0
    ? path.resolve(args[outIdx + 1])
    : path.join(FT_EXPORT_DIR, `debate-${todayStr()}.jsonl`);

  if (!fs.existsSync(TURNS_DIR)) {
    console.error(`data/worker-turns/ が見つかりません: ${TURNS_DIR}`);
    process.exit(1);
  }

  const turnFiles = fs.readdirSync(TURNS_DIR)
    .filter((f) => f.endsWith(".json") && !f.endsWith(".reply.json"))
    .map((f) => path.join(TURNS_DIR, f));

  let exported = 0;
  let skipped = 0;
  const lines: string[] = [];

  for (const turnFile of turnFiles) {
    const turn = loadJson<TurnJson>(turnFile);
    if (!turn?.reqId || !turn.system || !turn.prompt) {
      skipped++;
      continue;
    }

    const reply = findReply(turn.reqId);
    if (!reply) {
      skipped++;
      continue;
    }

    const sample = buildSample(turn, reply);
    if (!sample) {
      skipped++;
      continue;
    }

    lines.push(JSON.stringify(sample));
    exported++;
  }

  console.log(`\nスキャン: ${turnFiles.length} turns / 有効: ${exported} / スキップ: ${skipped}`);

  if (dryRun) {
    console.log("(dry-run) 書き込みは行いません");
    return;
  }

  if (exported === 0) {
    console.log("出力データなし。export-ft-data は何も書き込みませんでした。");
    return;
  }

  fs.mkdirSync(path.dirname(outFile), { recursive: true });
  fs.writeFileSync(outFile, lines.join("\n") + "\n", "utf-8");
  console.log(`出力: ${outFile} (${exported} samples)`);
}

main();
