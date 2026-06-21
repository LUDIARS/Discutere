/**
 * worker-pool 経路の LLM usage 回収 (#135) の純粋ロジック。
 *
 * 常駐ワーカー (Lictor がラップする Claude Code セッション) は utterance callback で
 * usage を返さない。代わりに Lictor が `--session-id` で固定した transcript JSONL
 * (`LICTOR_TRANSCRIPT_FILE`) の assistant メッセージ `message.usage` を読む。
 *
 * llm_call_log は「呼び出しごと 1 行」で、cost-report の集計 (costFeedRows) は
 * 行を SUM する。よって 1 ターンで報告するのは **累積ではなく「前回 send 以降に
 * 増えた assistant usage 行のデルタ」**でなければ二重計上になる。各 assistant 行を
 * 一度だけ計上するため、既計上の usage 持ち assistant 行数をカーソルで持ち越す。
 *
 * 既知の制約 (#135):
 *  - **cost_usd は取れない**: Claude Code transcript の `message.usage` は token のみ
 *    (input/output/cache) で total_cost_usd を行に持たない。worker-pool は token のみ。
 *  - **per-turn lag**: send 実行時点で当該ターンの usage 行が未フラッシュなら次回に回る
 *    (デルタ方式なので落とさず次の send で拾う)。
 */

import { readFileSync, writeFileSync } from "node:fs";

/**
 * transcript 全文 + 既計上 assistant 行数から「未計上分の usage 合算」を取り出す。
 *
 * @param {string} transcriptText  transcript JSONL の全文
 * @param {number} alreadyCounted  前回までに計上済みの usage 持ち assistant 行数
 * @returns {{ usage: object|null, count: number }}
 *   usage = 未計上 assistant の token 合算 (新規が無ければ null)。assistant
 *           `message.model` が在れば `usage.model` に最後の fresh 行のモデルを載せる
 *           (cost 補完がモデル単価を引けるようにする)。
 *   count = 現時点での usage 持ち assistant 行の総数 (次回カーソル)。
 */
export function usageDeltaFromTranscript(transcriptText, alreadyCounted) {
  /** @type {{usage: object, model: string|undefined}[]} */
  const lines = [];
  for (const line of transcriptText.split("\n")) {
    const t = line.trim();
    if (!t) continue;
    let msg;
    try {
      msg = JSON.parse(t);
    } catch {
      continue; // 壊れた行は無視
    }
    if (!msg || msg.type !== "assistant") continue;
    const u = msg.message?.usage;
    if (u && typeof u === "object") {
      const model = typeof msg.message?.model === "string" ? msg.message.model : undefined;
      lines.push({ usage: u, model });
    }
  }

  const total = lines.length;
  const start =
    Number.isFinite(alreadyCounted) && alreadyCounted > 0 ? Math.min(alreadyCounted, total) : 0;
  const fresh = lines.slice(start);
  if (fresh.length === 0) return { usage: null, count: total };

  const sum = {
    input_tokens: 0,
    output_tokens: 0,
    cache_read_input_tokens: 0,
    cache_creation_input_tokens: 0,
  };
  let any = false;
  const add = (key, v) => {
    if (typeof v === "number" && Number.isFinite(v)) {
      sum[key] += v;
      any = true;
    }
  };
  let model; // fresh 行で最後に観測した model (1 ターンは単一モデル想定、最後を採る)
  for (const { usage: u, model: m } of fresh) {
    add("input_tokens", u.input_tokens);
    add("output_tokens", u.output_tokens);
    add("cache_read_input_tokens", u.cache_read_input_tokens);
    add("cache_creation_input_tokens", u.cache_creation_input_tokens);
    if (m) model = m;
  }
  if (!any) return { usage: null, count: total };
  if (model) sum.model = model;
  return { usage: sum, count: total };
}

/** カーソルファイル (整数 1 行) を読む。読めなければ 0。best-effort。 */
export function readUsageCursor(path) {
  try {
    const n = parseInt(readFileSync(path, "utf8").trim(), 10);
    return Number.isFinite(n) && n >= 0 ? n : 0;
  } catch {
    return 0;
  }
}

/** カーソルファイルに count を書く。best-effort (失敗は次回の再計上を招くだけ)。 */
export function writeUsageCursor(path, count) {
  try {
    writeFileSync(path, String(count), "utf8");
  } catch {
    /* best-effort */
  }
}
