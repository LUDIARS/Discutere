// cost-report.ts の Node 実装の入出力を記録する (合成した llm_call_log を better-sqlite3 で集計)。
// 実行: npx tsx native/tests/golden/w1-E-cost.gen.ts → native/tests/fixtures/flow/w1e_cost.json
import fs from "node:fs";
import path from "node:path";
import Database from "better-sqlite3";
import { costFeedRows, formatCostReport, summarizeCost } from "../../../src/flow/cost-report.js";

// C++ テストと同じ合成スキーマ (flow の llm_call_log と同じ列)
export const SCHEMA = `CREATE TABLE llm_call_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT, flow TEXT NOT NULL, session_id TEXT NOT NULL, round INTEGER, turn INTEGER,
  role TEXT, persona TEXT, location TEXT NOT NULL, model TEXT, backend TEXT, latency_ms INTEGER,
  input_tokens INTEGER, output_tokens INTEGER, prompt TEXT, response TEXT, created_at INTEGER NOT NULL,
  cache_read_input_tokens INTEGER, cache_creation_input_tokens INTEGER, cost_usd REAL)`;
const rows: Array<[string, string, string | null, string | null, number | null, number | null, number | null, number | null, number | null, number]> = [
  ["discussion", "S1", "opus", "claude-cli", 10, 4, 100, 20, 0.01, 1000],
  ["discussion", "S1", "opus", "claude-cli", 10, 4, 100, 20, 0.02, 2000],
  ["improvement", "S2", "haiku", "anthropic", 7, 3, null, null, null, 3000],
  ["discussion", "S3長いセッション名でパディングを超えるかもしれないもの", null, null, 123456789, 1, 0, 0, 1.03125, 4000],
  ["learning", "S4", "sonnet", "claude-cli", 5, 5, 5, 5, 0.333333, 5000],
];
const db = new Database(":memory:");
db.exec(SCHEMA);
const ins = db.prepare(`INSERT INTO llm_call_log (flow, session_id, model, backend, input_tokens, output_tokens,
  cache_read_input_tokens, cache_creation_input_tokens, cost_usd, created_at, location) VALUES (?,?,?,?,?,?,?,?,?,?,'loc')`);
for (const r of rows) ins.run(...r);
const filters = [{}, { sessionId: "S1" }, { since: 2500 }, { activeSince: 1500 }, { activeSince: 99999 }, { sessionId: "" }];
const out = {
  schema: SCHEMA,
  rows,
  cases: filters.map((filter) => {
    const report = summarizeCost(db, filter);
    return { filter, report, feed: costFeedRows(db, filter), text: formatCostReport(report) };
  }),
};
const dir = path.join(path.dirname(new URL(import.meta.url).pathname), "..", "fixtures", "flow");
fs.writeFileSync(path.join(dir, "w1e_cost.json"), JSON.stringify(out, null, 2) + "\n");
console.log("wrote w1e_cost.json");
