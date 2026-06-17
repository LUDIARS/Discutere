/**
 * llm_call_log のコスト/トークン集計 (内部検証用)。
 *
 * セッション別・フロー別・backend 別に呼び出し回数 / トークン / 推定コストを合算する。
 * cost_usd は claude-cli(サブスク) では等価 API 換算の推定値 (実課金ではない)、
 * anthropic 従量経路では通常 NULL。トークン (input/output/cache) は backend に依存。
 *
 * CLI から使う: `npm run cost-report`(scripts/cost-report.ts)。
 */

import type Database from "better-sqlite3";

export interface CostRow {
  /** 集計キー (session_id / flow / backend のいずれか)。 */
  key: string;
  calls: number;
  inputTokens: number;
  outputTokens: number;
  cacheReadTokens: number;
  cacheCreationTokens: number;
  costUsd: number;
}

export interface CostReport {
  total: CostRow;
  bySession: CostRow[];
  byFlow: CostRow[];
  byBackend: CostRow[];
}

export interface CostReportFilter {
  /** 単一セッションに絞る。 */
  sessionId?: string;
  /** 行を created_at >= since (epoch ms) で絞る。 */
  since?: number;
  /**
   * since 以降に活動した「セッション」を対象に、そのセッションの全行を含める
   * (= 累積サマリを保ったまま最近活動分だけ送る relay 用)。`since` の行フィルタとは別。
   */
  activeSince?: number;
}

interface RawAgg {
  key: string | null;
  calls: number;
  input_tokens: number | null;
  output_tokens: number | null;
  cache_read_input_tokens: number | null;
  cache_creation_input_tokens: number | null;
  cost_usd: number | null;
}

const SELECT_SUMS = `
  COUNT(*) AS calls,
  COALESCE(SUM(input_tokens), 0) AS input_tokens,
  COALESCE(SUM(output_tokens), 0) AS output_tokens,
  COALESCE(SUM(cache_read_input_tokens), 0) AS cache_read_input_tokens,
  COALESCE(SUM(cache_creation_input_tokens), 0) AS cache_creation_input_tokens,
  COALESCE(SUM(cost_usd), 0) AS cost_usd`;

function toRow(r: RawAgg, fallbackKey: string): CostRow {
  return {
    key: r.key ?? fallbackKey,
    calls: r.calls,
    inputTokens: r.input_tokens ?? 0,
    outputTokens: r.output_tokens ?? 0,
    cacheReadTokens: r.cache_read_input_tokens ?? 0,
    cacheCreationTokens: r.cache_creation_input_tokens ?? 0,
    costUsd: r.cost_usd ?? 0,
  };
}

function whereClause(filter: CostReportFilter): { sql: string; params: unknown[] } {
  const conds: string[] = [];
  const params: unknown[] = [];
  if (filter.sessionId) {
    conds.push("session_id = ?");
    params.push(filter.sessionId);
  }
  if (typeof filter.since === "number") {
    conds.push("created_at >= ?");
    params.push(filter.since);
  }
  if (typeof filter.activeSince === "number") {
    conds.push("session_id IN (SELECT session_id FROM llm_call_log WHERE created_at >= ?)");
    params.push(filter.activeSince);
  }
  return { sql: conds.length ? `WHERE ${conds.join(" AND ")}` : "", params };
}

/** llm_call_log を集計して CostReport を返す。 */
export function summarizeCost(
  db: Database.Database,
  filter: CostReportFilter = {}
): CostReport {
  const { sql: where, params } = whereClause(filter);

  const total = db
    .prepare(`SELECT NULL AS key, ${SELECT_SUMS} FROM llm_call_log ${where}`)
    .get(...params) as RawAgg;

  const groupBy = (col: string): CostRow[] =>
    (
      db
        .prepare(
          `SELECT ${col} AS key, ${SELECT_SUMS} FROM llm_call_log ${where}
           GROUP BY ${col} ORDER BY cost_usd DESC, calls DESC`
        )
        .all(...params) as RawAgg[]
    ).map((r) => toRow(r, "(none)"));

  return {
    total: toRow(total, "(total)"),
    bySession: groupBy("session_id"),
    byFlow: groupBy("flow"),
    byBackend: groupBy("backend"),
  };
}

/** Anatomia cost-feed への PUSH 1 行 = (session × model × backend) 別サマリ。 */
export interface CostFeedRow {
  sessionId: string;
  model?: string;
  backend?: string;
  calls: number;
  inputTokens: number;
  outputTokens: number;
  cacheReadTokens: number;
  cacheCreationTokens: number;
  costUsd: number;
}

interface RawFeedAgg extends Omit<RawAgg, "key"> {
  session_id: string;
  model: string | null;
  backend: string | null;
}

/**
 * llm_call_log を (session_id, model, backend) 別に集計し、Anatomia の
 * POST /api/cost-feed が受け取る行形に変換する。filter で since/session を絞れる。
 */
export function costFeedRows(
  db: Database.Database,
  filter: CostReportFilter = {}
): CostFeedRow[] {
  const { sql: where, params } = whereClause(filter);
  const rows = db
    .prepare(
      `SELECT session_id, model, backend, ${SELECT_SUMS}
       FROM llm_call_log ${where}
       GROUP BY session_id, model, backend
       ORDER BY session_id`
    )
    .all(...params) as RawFeedAgg[];
  return rows.map((r) => ({
    sessionId: r.session_id,
    model: r.model ?? undefined,
    backend: r.backend ?? undefined,
    calls: r.calls,
    inputTokens: r.input_tokens ?? 0,
    outputTokens: r.output_tokens ?? 0,
    cacheReadTokens: r.cache_read_input_tokens ?? 0,
    cacheCreationTokens: r.cache_creation_input_tokens ?? 0,
    costUsd: r.cost_usd ?? 0,
  }));
}

/** CostReport を人間可読の文字列にする (CLI 出力用)。 */
export function formatCostReport(report: CostReport): string {
  const usd = (n: number): string => `$${n.toFixed(4)}`;
  const line = (r: CostRow): string =>
    `  ${r.key.padEnd(28)} calls=${String(r.calls).padStart(5)} ` +
    `in=${String(r.inputTokens).padStart(8)} out=${String(r.outputTokens).padStart(7)} ` +
    `cacheR=${String(r.cacheReadTokens).padStart(9)} cacheW=${String(r.cacheCreationTokens).padStart(8)} ` +
    `cost=${usd(r.costUsd)}`;

  const section = (title: string, rows: CostRow[]): string =>
    [`${title}:`, ...rows.map(line)].join("\n");

  const t = report.total;
  return [
    "=== Discutere LLM cost report ===",
    `TOTAL  calls=${t.calls} in=${t.inputTokens} out=${t.outputTokens} ` +
      `cacheRead=${t.cacheReadTokens} cacheCreate=${t.cacheCreationTokens} cost=${usd(t.costUsd)}`,
    "(cost_usd: claude-cli=等価API換算の推定 / anthropic従量=未取得でNULL=0)",
    "",
    section("by backend", report.byBackend),
    "",
    section("by flow", report.byFlow),
    "",
    section("by session", report.bySession),
  ].join("\n");
}
