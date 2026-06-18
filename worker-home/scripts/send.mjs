#!/usr/bin/env node
/**
 * 1 ターンの発話送信。
 *
 * reply JSON ファイルをそのまま (UTF-8 バイト列のまま) POST する。
 * 旧 curl `--data-binary @file` と同じ「ファイルを生で送る」挙動で日本語 mojibake を回避する。
 *
 * usage: node scripts/send.mjs <reply.json への相対 or 絶対パス>
 * 環境変数:
 *   - DI_CALLBACK_URL : Discutere の base URL (例 http://127.0.0.1:3100)
 *   - LICTOR_TRANSCRIPT_FILE : (任意) Lictor が --session-id で固定した transcript JSONL。
 *     在れば assistant usage のデルタを reply body の `usage` に載せて送る (#135)。
 */

import { readFile } from "node:fs/promises";
import { existsSync, readFileSync } from "node:fs";

import { usageDeltaFromTranscript, readUsageCursor, writeUsageCursor } from "./usage.mjs";

const base = (process.env.DI_CALLBACK_URL ?? "").replace(/\/+$/, "");
const file = process.argv[2];

if (!base || !file) {
  console.error(`[send] usage: node scripts/send.mjs <replyFile>  (DI_CALLBACK_URL required, got "${base || "(empty)"}")`);
  process.exit(1);
}

let body = await readFile(file).catch((err) => {
  console.error(`[send] cannot read ${file}: ${err.message}`);
  process.exit(1);
});

// worker-pool 経路の usage 回収 (#135): transcript が固定されていれば、前回 send 以降に
// 増えた assistant usage を合算して body.usage に載せる。カーソルの前進は POST 成功後に行い、
// 失敗時に取りこぼさない (at-least-once)。usage 添付時のみ body を JSON 再エンコードする
// (それ以外は raw Buffer 送出を保ち mojibake 経路を温存)。
const transcriptFile = process.env.LICTOR_TRANSCRIPT_FILE;
let cursorAdvance = null; // { path, count }
if (transcriptFile && existsSync(transcriptFile)) {
  try {
    const cursorPath = `${transcriptFile}.di-usage-cursor`;
    const prev = readUsageCursor(cursorPath);
    const { usage, count } = usageDeltaFromTranscript(readFileSync(transcriptFile, "utf8"), prev);
    if (usage) {
      const obj = JSON.parse(body.toString("utf8"));
      obj.usage = usage;
      body = Buffer.from(JSON.stringify(obj), "utf8");
      cursorAdvance = { path: cursorPath, count };
    }
  } catch (err) {
    console.error(`[send] usage attach skipped: ${err.message}`);
  }
}

const res = await fetch(`${base}/internal/worker/utterance`, {
  method: "POST",
  headers: { "content-type": "application/json" },
  body, // Buffer をそのまま送る (UTF-8 保持)
}).catch((err) => {
  console.error(`[send] fetch failed: ${err.message}`);
  process.exit(1);
});

if (!res.ok) {
  const text = await res.text().catch(() => "");
  console.error(`[send] HTTP ${res.status}: ${text.slice(0, 200)}`);
  process.exit(1);
}

// POST 成功後にカーソルを前進 (このデルタを計上済みとする)。
if (cursorAdvance) writeUsageCursor(cursorAdvance.path, cursorAdvance.count);

console.log(`[send] ok ${file}`);
