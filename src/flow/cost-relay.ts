/**
 * LLM コストの横断 relay (Push 型集約)。
 *
 * llm_call_log を (session × model × backend) 別に集計し、コスト表示面 (Anatomia /
 * Concordia) の cost-feed エンドポイントへ PUSH する。受信側は
 * (service,session,model,backend) で latest-wins dedupe するので、各 PUSH は
 * 「そのセッションの累積サマリ」でなければならない。よって定期 relay は activeSince で
 * 「最近活動したセッション」を選び、そのセッションの全行を集計して送る (delta ではなく累積)。
 *
 * **複数サービスへ同時 push** する: 同じ集計を全 target に書き込む (push 先複数化)。
 * Anatomia と Concordia でエンドポイントのパスが異なる ({@link COST_FEED_PATHS}) ため、
 * target は baseUrl + path を持つ。送信失敗は議論を止めない (graceful、ログのみ)。
 *
 * SRP: 集計→HTTP push と定期実行のみ。集計 SQL は cost-report.ts。
 */

import type Database from "better-sqlite3";
import { costFeedRows, type CostFeedRow } from "./cost-report.js";

type FetchLike = (input: string, init: RequestInit) => Promise<Response>;

/** 各コスト表示面の cost-feed エンドポイントのパス (受信側 API で固定)。 */
export const COST_FEED_PATHS = {
  anatomia: "/api/cost-feed",
  concordia: "/v1/cost-feed",
} as const;

/** push 先 1 つ。baseUrl + path で完全なエンドポイントを成す。 */
export interface CostFeedTarget {
  /** ベース URL (例 http://localhost:4200)。末尾スラッシュは無視。 */
  baseUrl: string;
  /** cost-feed エンドポイントのパス ({@link COST_FEED_PATHS} 参照)。 */
  path: string;
  /** ログ表示用ラベル (例 "anatomia")。省略時は baseUrl。 */
  label?: string;
}

/**
 * 既知のサービス URL から target 配列を組み立てる (設定 → target の変換)。
 * 指定された URL の分だけ target を作る。両方指定すれば両方へ push。
 */
export function buildCostFeedTargets(urls: {
  anatomiaUrl?: string;
  concordiaUrl?: string;
}): CostFeedTarget[] {
  const targets: CostFeedTarget[] = [];
  if (urls.anatomiaUrl) {
    targets.push({ baseUrl: urls.anatomiaUrl, path: COST_FEED_PATHS.anatomia, label: "anatomia" });
  }
  if (urls.concordiaUrl) {
    targets.push({ baseUrl: urls.concordiaUrl, path: COST_FEED_PATHS.concordia, label: "concordia" });
  }
  return targets;
}

export interface PushResult {
  ok: boolean;
  recorded?: number;
  error?: string;
}

/** rows を 1 つの target の cost-feed エンドポイントに送る。例外は ok:false に畳む。 */
export async function pushCostFeed(
  target: CostFeedTarget,
  service: string,
  rows: CostFeedRow[],
  ts: number,
  fetchImpl: FetchLike = fetch,
): Promise<PushResult> {
  const endpoint = target.baseUrl.replace(/\/+$/, "") + target.path;
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
  /** push 先 (複数)。空なら何もしない。 */
  targets: CostFeedTarget[];
  service: string;
  /** since 以降に活動したセッションのみ送る (未指定なら全セッション)。 */
  activeSince?: number;
  /** 送信時刻 (epoch ms)。テスト用に注入可。 */
  ts: number;
  fetchImpl?: FetchLike;
}

/** target 別の push 結果。 */
export interface TargetPushResult {
  target: CostFeedTarget;
  push: PushResult;
}

export interface RelayResult {
  /** 集計して送ろうとしたセッション行数 (0 = 送るものなし)。 */
  pushed: number;
  /** target 別の結果。 */
  results: TargetPushResult[];
  /** 全 target 成功か (rows=0 なら true)。 */
  ok: boolean;
}

/**
 * 一回分の relay: 行を一度だけ集計し、全 target へ同じ payload を push する。
 * 送るものが無ければ pushed:0 / results:[] / ok:true。
 */
export async function relayCostOnce(
  db: Database.Database,
  opts: RelayOnceOptions,
): Promise<RelayResult> {
  const rows = costFeedRows(
    db,
    opts.activeSince != null ? { activeSince: opts.activeSince } : {},
  );
  if (rows.length === 0 || opts.targets.length === 0) {
    return { ok: true, pushed: 0, results: [] };
  }
  const results: TargetPushResult[] = [];
  for (const target of opts.targets) {
    const push = await pushCostFeed(target, opts.service, rows, opts.ts, opts.fetchImpl);
    results.push({ target, push });
  }
  return { ok: results.every((r) => r.push.ok), pushed: rows.length, results };
}

/** 毎 tick 評価される実効設定 (設定 UI からのライブ変更を反映する)。 */
export interface CostRelayLive {
  /** false なら push せず cursor だけ near-now に保つ (有効化時に backlog を出さない)。 */
  enabled: boolean;
  /** push 先 (複数)。空なら push しない。 */
  targets: CostFeedTarget[];
  /** cost-feed の service ラベル。 */
  service: string;
}

export interface StartCostRelayOptions {
  /**
   * 毎 tick 呼ぶ設定リゾルバ。enabled / targets / service を返す。
   * runtime-settings から読むことで、再起動なしで UI の変更が次 tick に効く。
   */
  resolve: () => CostRelayLive;
  /** timer 周期 ms (起動時固定。変更は再起動)。 */
  intervalMs: number;
  /** 失敗ログ用 (既定 console.error)。 */
  onError?: (msg: string) => void;
  /** epoch ms を返す関数 (テスト用)。既定 Date.now。 */
  now?: () => number;
  fetchImpl?: FetchLike;
}

/**
 * 定期 relay を開始する。返り値を呼ぶと停止。timer は常に回り、各 tick で
 * {@link StartCostRelayOptions.resolve} を評価する (= UI でのライブ有効化/無効化に対応)。
 * 各 tick は「前回 tick から 1 interval 重ねた時刻」以降に活動したセッションを
 * 全 target へ送る (取りこぼし回避)。初回は intervalMs×4 分さかのぼる。
 * カーソル前進は全 target 成功時のみ — 一部失敗した window は次 tick で再送する
 * (累積 push なので再送は idempotent)。無効/送信先なしの間は cursor を near-now に
 * 保ち、有効化した瞬間に過去の backlog を一気に送らない。
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
    const live = opts.resolve();
    if (!live.enabled || live.targets.length === 0) {
      nextSince = start - opts.intervalMs; // 無効中は cursor を near-now に保つ
      return;
    }
    const since = nextSince;
    const r = await relayCostOnce(db, {
      targets: live.targets,
      service: live.service,
      activeSince: since,
      ts: start,
      fetchImpl: opts.fetchImpl,
    });
    for (const { target, push } of r.results) {
      if (!push.ok) onError(`push to ${target.label ?? target.baseUrl} failed: ${push.error}`);
    }
    if (r.ok) {
      nextSince = start - opts.intervalMs; // 次回は 1 interval 重ねる
    }
  };

  void tick();
  const timer = setInterval(() => void tick(), opts.intervalMs);
  if (typeof (timer as { unref?: () => void }).unref === "function") {
    (timer as { unref: () => void }).unref();
  }
  return () => clearInterval(timer);
}
