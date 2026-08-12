/**
 * FT 教師データエクスポート — spec/feature/gemma-ft.md (task #138)。
 *
 * data/worker-turns/*.json (turn) と対応する *.reply.json をスキャンし、
 * OpenAI chat JSONL 形式 (system/user/assistant) に変換して出力する。
 *
 * 出力先: data/ft-export/debate-YYYY-MM-DD.jsonl
 *
 * 使い方:
 *   npx tsx scripts/export-ft-data.ts               # 全件エクスポート (品質フィルタあり)
 *   npx tsx scripts/export-ft-data.ts --dry-run     # 件数確認のみ
 *   npx tsx scripts/export-ft-data.ts --out path    # 出力先指定
 *   npx tsx scripts/export-ft-data.ts --no-filter   # 品質フィルタ無効 (v1 互換の全量)
 *   npx tsx scripts/export-ft-data.ts --min-length 40 --group-cap 200
 *
 * 品質フィルタ (spec/feature/gemma-ft.md §9-1): teacher の失敗ターン (浅い相槌・
 * 繰り返し・system 復唱) を決定的に除外する。除外は理由別に集計ログへ出す。
 */

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { createQualityFilter, type RejectReason } from "./ft/quality-filter.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const TURNS_DIR = path.resolve(HERE, "../data/worker-turns");
const WORKER_HOME_REPLIES = path.resolve(HERE, "../worker-home/replies");
const FT_EXPORT_DIR = path.resolve(HERE, "../data/ft-export");

interface TurnJson {
  reqId: string;
  workerId: string;
  sessionId?: string;
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

function loadJson(filePath: string): unknown {
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf-8")) as unknown;
  } catch {
    return null;
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function isTurnJson(value: unknown): value is TurnJson {
  return (
    isRecord(value) &&
    typeof value.reqId === "string" &&
    typeof value.workerId === "string" &&
    (value.sessionId === undefined || typeof value.sessionId === "string") &&
    typeof value.system === "string" &&
    typeof value.prompt === "string"
  );
}

function isReplyJson(value: unknown): value is ReplyJson {
  return (
    isRecord(value) &&
    typeof value.reqId === "string" &&
    typeof value.workerId === "string" &&
    typeof value.text === "string"
  );
}

/** reqId に対応する reply を探す。turns dir → worker-home/replies の順で探す。 */
function findReply(reqId: string): ReplyJson | null {
  const inTurns = loadJson(path.join(TURNS_DIR, `${reqId}.reply.json`));
  if (isReplyJson(inTurns)) return inTurns;
  const inWorkerHome = loadJson(path.join(WORKER_HOME_REPLIES, `${reqId}.reply.json`));
  return isReplyJson(inWorkerHome) ? inWorkerHome : null;
}

function buildSample(turn: TurnJson, reply: ReplyJson): ChatSample | null {
  const text = (reply.text ?? "").trim();
  if (!text) return null;

  // JSON-only response (e.g. {action:"skip"}) は学習対象から除外する
  if (text.startsWith("{") && text.includes('"action"')) {
    try {
      const parsed = JSON.parse(text) as unknown;
      if (isRecord(parsed) && parsed.action === "skip") return null;
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
  const outArg = outIdx >= 0 ? args[outIdx + 1] : undefined;
  if (outIdx >= 0 && (!outArg || outArg.startsWith("--"))) {
    console.error("--out には出力先パスを指定する");
    process.exit(1);
  }
  const outFile = outArg
    ? path.resolve(outArg)
    : path.join(FT_EXPORT_DIR, `debate-${todayStr()}.jsonl`);

  if (!fs.existsSync(TURNS_DIR)) {
    console.error("data/worker-turns/ が見つかりません");
    process.exit(1);
  }

  const turnFiles = fs.readdirSync(TURNS_DIR)
    .filter((f) => f.endsWith(".json") && !f.endsWith(".reply.json"))
    // 品質フィルタは直前採用ターンと採用数を保持するため、OS の readdir 順に依存させない。
    .sort()
    .map((f) => path.join(TURNS_DIR, f));

  const noFilter = args.includes("--no-filter");
  const numArg = (name: string, dflt: number): number => {
    const i = args.indexOf(name);
    if (i < 0) return dflt;
    const n = Number(args[i + 1]);
    if (!Number.isInteger(n) || n < 0) {
      console.error(`${name} には 0 以上の整数を指定する`);
      process.exit(1);
    }
    return n;
  };
  const qualityFilter = createQualityFilter({
    minLength: numArg("--min-length", 40),
    groupCap: numArg("--group-cap", 0),
  });

  let exported = 0;
  let skipped = 0;
  const rejectedByReason = new Map<RejectReason, number>();
  const lines: string[] = [];

  for (const turnFile of turnFiles) {
    const loadedTurn = loadJson(turnFile);
    const fileReqId = path.basename(turnFile, ".json");
    if (
      !isTurnJson(loadedTurn) ||
      loadedTurn.reqId !== fileReqId ||
      !loadedTurn.reqId ||
      !loadedTurn.workerId ||
      !loadedTurn.system ||
      !loadedTurn.prompt
    ) {
      skipped++;
      continue;
    }
    const turn = loadedTurn;

    // JSON 内の reqId をパスへ使わず、readdir 由来の basename を正本にする。
    const reply = findReply(fileReqId);
    if (!reply || reply.reqId !== turn.reqId || reply.workerId !== turn.workerId) {
      skipped++;
      continue;
    }

    const sample = buildSample(turn, reply);
    if (!sample) {
      skipped++;
      continue;
    }

    if (!noFilter) {
      const verdict = qualityFilter({
        reqId: turn.reqId,
        workerId: turn.workerId,
        sessionId:
          typeof turn.sessionId === "string" && turn.sessionId.trim().length > 0
            ? turn.sessionId
            : undefined,
        system: turn.system,
        assistant: sample.messages[2].content,
      });
      if (!verdict.accepted && verdict.reason) {
        rejectedByReason.set(verdict.reason, (rejectedByReason.get(verdict.reason) ?? 0) + 1);
        continue;
      }
    }

    lines.push(JSON.stringify(sample));
    exported++;
  }

  const rejectedTotal = [...rejectedByReason.values()].reduce((a, b) => a + b, 0);
  console.log(
    `\nスキャン: ${turnFiles.length} turns / 有効: ${exported} / スキップ: ${skipped}` +
      ` / 品質除外: ${rejectedTotal}${noFilter ? " (フィルタ無効)" : ""}`
  );
  // silent drop にしない: 除外は理由別に必ず出す (§9-1)。
  for (const [reason, count] of rejectedByReason) {
    console.log(`  除外 (${reason}): ${count} 件`);
  }

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
  console.log(`出力: ${path.basename(outFile)} (${exported} samples)`);
}

main();
