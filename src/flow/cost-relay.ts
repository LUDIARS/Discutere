/**
 * LLM コストの Anatomia への relay (Push 型集約)。
 *
 * llm_call_log を (session × model × backend) 別に集計し、Anatomia の
 * POST /api/cost-feed へ PUSH する。Anatomia は (service,session,model,backend)
 * で latest-wins dedupe するので、各 PUSH は「そのセッションの累積サマリ」で
 * なければならない。よって定期 relay は activeSince で「最近活動したセッション」
 * を選び、そのセッションの全行を集計して送る (delta ではなく累積)。
 *
 * 送信失敗は議論を止めない (graceful、ログのみ)。
 *
 * SRP: 集計→HTTP push と定期実行のみ。集計 SQL は cost-report.ts。
 */

import type Database from "better-sqlite3";
import { costFeedRows, type CostFeedRow } from "./cost-report.js";

type FetchLike = (input: string, init: RequestInit) => Promise<Response>;

export interface PushResult {
  ok: boolean;
  recorded?: number;
  error?: string;
}

/** rows を Anatomia の POST /api/cost-feed に送る。例外は ok:false に畳む。 */
export async function pushCostFeed(
  baseUrl: string,
  service: string,
  rows: CostFeedRow[],
  ts: number,
  fetchImpl: FetchLike = fetch,
): Promise<PushResult> {
  const endpoint = baseUrl.replace(/\/+$/, "") + "/api/cost-feed";
  try {
    const res = await fetchImpl(endpoint, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ service, ts, sessions: rows }),
    });
    if (!res.ok) return { ok: false, error: `http ${res.status}` };
    const json = (await res.json().catch(() => ({}))) as { recorded?: number };
    return { ok: true, recorded: json.recorded };
  } catch (e) {
    return { ok: false, error: (e as Error).message };
  }
}

export interface RelayOnceOptions {
  baseUrl: string;
  service: string;
  /** since 以降に活動したセッションのみ送る (未指定なら全セッション)。 */
  activeSince?: number;
  /** 送信時刻 (epoch ms)。テスト用に注入可。 */
  ts: number;
  fetchImpl?: FetchLike;
}

export interface RelayResult extends PushResult {
  /** 送ったセッション行数 (0 = 送るものなし)。 */
  pushed: number;
}

/** 一回分の relay: 行を集計して push する。送るものが無ければ pushed:0 で成功。 */
export async function relayCostOnce(
  db: Database.Database,
  opts: RelayOnceOptions,
): Promise<RelayResult> {
  const rows = costFeedRows(
    db,
    opts.activeSince != null ? { activeSince: opts.activeSince } : {},
  );
  if (rows.length === 0) return { ok: true, pushed: 0 };
  const res = await pushCostFeed(opts.baseUrl, opts.service, rows, opts.ts, opts.fetchImpl);
  return { ...res, pushed: res.ok ? rows.length : 0 };
}

export interface StartCostRelayOptions {
  baseUrl: string;
  service: string;
  intervalMs: number;
  /** 失敗ログ用 (既定 console.error)。 */
  onError?: (msg: string) => void;
  /** epoch ms を返す関数 (テスト用)。既定 Date.now。 */
  now?: () => number;
  fetchImpl?: FetchLike;
}

/**
 * 定期 relay を開始する。返り値を呼ぶと停止。
 * 各 tick は「前回 tick から 1 interval 重ねた時刻」以降に活動したセッションを送る
 * (取りこぼし回避)。初回は intervalMs×4 分さかのぼる。
 */
export function startCostRelay(
  db: Database.Database,
  opts: StartCostRelayOptions,
): () => void {
  const now = opts.now ?? Date.now;
  const onError = opts.onError ?? ((m) => console.error(`[cost-relay] ${m}`));
  let nextSince = now() - opts.intervalMs * 4;

  const tick = async (): Promise<void> => {
    const start = now();
    const since = nextSince;
    const r = await relayCostOnce(db, {
      baseUrl: opts.baseUrl,
      service: opts.service,
      activeSince: since,
      ts: start,
      fetchImpl: opts.fetchImpl,
    });
    if (r.ok) {
      nextSince = start - opts.intervalMs; // 次回は 1 interval 重ねる
    } else {
      onError(`push failed: ${r.error}`);
    }
  };

  void tick();
  const timer = setInterval(() => void tick(), opts.intervalMs);
  if (typeof (timer as { unref?: () => void }).unref === "function") {
    (timer as { unref: () => void }).unref();
  }
  return () => clearInterval(timer);
}
