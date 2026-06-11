/**
 * 常駐ワーカープール。
 *
 * - start(): 全ワーカーを spawn (standing prompt を書き出して auto-inject に乗せる)。
 * - registerPort(): ワーカーの自己 register を受けて sidecar port を記録。
 * - dispatch(): ターン JSON を書き出し、ワーカーの Lictor /v1/keys に 1 行注入。
 *               発話が callback (onUtterance) で戻るまでの Promise を返す。
 * - onUtterance(): callback 受信 → 対応する pending を resolve。
 * - stop(): 全ワーカーを kill。
 */

import { mkdirSync, writeFileSync } from "node:fs";
import { join, isAbsolute } from "node:path";

import { buildStandingPrompt } from "./persona-prompts.js";
import { killWorker, spawnWorker } from "./spawner.js";
import type { WorkerConfig, WorkerPoolConfig, WorkerRuntime } from "./types.js";

export interface PoolLogger {
  info: (msg: string, meta?: unknown) => void;
  warn: (msg: string, meta?: unknown) => void;
}

interface PendingTurn {
  workerId: string;
  resolve: (text: string) => void;
  reject: (err: Error) => void;
  timer: NodeJS.Timeout;
}

const defaultLogger: PoolLogger = {
  info: (m, meta) => console.log(`[worker-pool] ${m}`, meta ?? ""),
  warn: (m, meta) => console.warn(`[worker-pool] ${m}`, meta ?? ""),
};

/**
 * TUI へ本文を注入してから Enter (CR) を「別 write」で送るまでの待ち時間 (ms)。
 * 本文 + CR を 1 write で送ると Claude TUI が bracketed paste 扱いし、CR が入力欄の
 * 改行として取り込まれて submit されない (= プロンプトが入力欄に溜まり、ワーカーが
 * 一切応答しない / 発話 0)。本文 → 待機 → CR の 2 段送出で確実に submit させる。
 */
const SUBMIT_DELAY_MS = 150;

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export class WorkerPool {
  private readonly workers = new Map<string, WorkerRuntime>();
  private readonly pending = new Map<string, PendingTurn>();
  private readonly log: PoolLogger;
  private readonly absTurnsDir: string;
  private readonly absPromptsDir: string;

  constructor(
    private readonly cfg: WorkerPoolConfig,
    private readonly cwd: string,
    logger?: PoolLogger
  ) {
    this.log = logger ?? defaultLogger;
    this.absTurnsDir = isAbsolute(cfg.turnsDir) ? cfg.turnsDir : join(cwd, cfg.turnsDir);
    this.absPromptsDir = isAbsolute(cfg.promptsDir) ? cfg.promptsDir : join(cwd, cfg.promptsDir);
  }

  private ensureDirs(): void {
    mkdirSync(this.absTurnsDir, { recursive: true });
    mkdirSync(this.absPromptsDir, { recursive: true });
  }

  /** 1 ワーカーを spawn して runtime に登録 (既に起動済なら何もしない)。 */
  private spawnOne(worker: WorkerConfig): void {
    if (this.workers.has(worker.id)) return;
    const promptPath = join(this.absPromptsDir, `${worker.id}.md`);
    const body = buildStandingPrompt({ worker });
    writeFileSync(promptPath, body, "utf8");
    // ワーカーは worker-home を cwd にして起動 (専用 .claude/settings.json を効かせる)。
    const { pid } = spawnWorker({ worker, promptPath, cwd: this.cfg.workerCwd, cfg: this.cfg });
    this.workers.set(worker.id, {
      config: worker,
      pid,
      lictorPid: null,
      lictorPort: null,
      readyAt: null,
      settleTimer: null,
      busy: false,
      promptPath,
    });
    this.log.info(`spawned worker ${worker.id} (${worker.provider}/${worker.model}) pid=${pid}`);
  }

  /** 設定された全ワーカーを spawn する (起動済はスキップ)。 */
  start(): void {
    this.ensureDirs();
    for (const worker of this.cfg.workers) this.spawnOne(worker);
  }

  /** 指定 id のワーカーを 1 体起動。config に無い id は false。 */
  startWorker(id: string): boolean {
    const w = this.cfg.workers.find((x) => x.id === id);
    if (!w) return false;
    this.ensureDirs();
    this.spawnOne(w);
    return true;
  }

  /** 指定 id のワーカーを 1 体停止 (プロセス kill + runtime 除去)。 */
  stopWorker(id: string): boolean {
    const rt = this.workers.get(id);
    if (!rt) return false;
    if (rt.settleTimer) clearTimeout(rt.settleTimer);
    killWorker(rt.pid, rt.lictorPid);
    this.workers.delete(id);
    this.log.info(`stopped worker ${id} (pid=${rt.pid} lictorPid=${rt.lictorPid ?? "-"})`);
    return true;
  }

  /** config に定義された全ワーカー (起動状態込み) を返す (UI 用)。 */
  listConfigured(): Array<{
    id: string;
    role: string;
    provider: string;
    model: string;
    running: boolean;
    registered: boolean;
    settling: boolean;
    busy: boolean;
    pid: number | null;
    port: number | null;
  }> {
    return this.cfg.workers.map((w) => {
      const rt = this.workers.get(w.id);
      return {
        id: w.id,
        role: w.role,
        provider: w.provider,
        model: w.model,
        running: !!rt,
        registered: rt?.readyAt != null,
        settling: rt != null && rt.lictorPort != null && rt.readyAt == null,
        busy: rt?.busy ?? false,
        pid: rt?.pid ?? null,
        port: rt?.lictorPort ?? null,
      };
    });
  }

  hasWorker(id: string): boolean {
    return this.workers.has(id);
  }

  /** personaId に対応する worker 定義 (provider/model)。config に無ければ null。 */
  getWorkerConfig(id: string): { id: string; role: string; provider: string; model: string } | null {
    return this.cfg.workers.find((w) => w.id === id) ?? null;
  }

  /** dispatch 可能か (settle 完了 = TUI が idle に戻っている)。 */
  isReady(id: string): boolean {
    return this.workers.get(id)?.readyAt != null;
  }

  /**
   * ワーカーの自己 register。port と lictorPid を記録し、settle timer を開始する。
   *
   * register.mjs の POST が来た時点でワーカーの TUI はまだ「登録完了」を生成中。
   * registerSettleMs 経過後に readyAt を設定して dispatch を解禁することで
   * busy な TUI へのキー注入を防ぐ (= turn timeout の根本原因)。
   */
  registerPort(workerId: string, lictorPort: number, lictorPid?: number): boolean {
    const rt = this.workers.get(workerId);
    if (!rt) {
      this.log.warn(`register from unknown worker ${workerId}`);
      return false;
    }
    if (rt.settleTimer) clearTimeout(rt.settleTimer);
    rt.lictorPort = lictorPort;
    rt.lictorPid = lictorPid ?? null;
    rt.readyAt = null;
    rt.settleTimer = setTimeout(() => {
      rt.readyAt = Date.now();
      rt.settleTimer = null;
      this.log.info(`worker ${workerId} idle-ready (settled after ${this.cfg.registerSettleMs}ms)`);
    }, this.cfg.registerSettleMs);
    this.log.info(
      `worker ${workerId} registered port=${lictorPort} lictorPid=${lictorPid ?? "-"} — settling ${this.cfg.registerSettleMs}ms`
    );
    return true;
  }

  /**
   * ターンをワーカーへ投げ、発話が callback で戻るまで待つ。
   * ターン本文は file に書き、Lictor /v1/keys には 1 行だけ注入する
   * (TUI へのマルチライン paste で誤 submit するのを避ける)。
   */
  async dispatch(
    workerId: string,
    payload: { reqId: string; system: string; prompt: string }
  ): Promise<string> {
    const rt = this.workers.get(workerId);
    if (!rt) throw new Error(`unknown worker ${workerId}`);
    if (rt.lictorPort == null) throw new Error(`worker ${workerId} not registered (no port)`);
    if (rt.readyAt == null) throw new Error(`worker ${workerId} still settling after register — retry after registerSettleMs`);
    if (rt.busy) throw new Error(`worker ${workerId} busy`);

    const turnPath = join(this.absTurnsDir, `${payload.reqId}.json`);
    writeFileSync(
      turnPath,
      JSON.stringify({ reqId: payload.reqId, workerId, system: payload.system, prompt: payload.prompt }, null, 2),
      "utf8"
    );

    // worker の cwd は worker-home なので turnsDir を相対で渡すと解決できない。
    // 絶対パス (forward slash 正規化) で注入する。worker は Read ツールで読む。
    const turnAbs = turnPath.replace(/\\/g, "/");
    // 本文と Enter(CR) を分けて送る。1 write (`...\r`) だと bracketed paste 扱いで
    // CR が改行化し submit されない (プロンプト蓄積 / 発話 0 の原因)。
    const line = `[TURN] ${payload.reqId} ${turnAbs}`;
    await this.injectKeys(rt.lictorPort, line);
    await delay(SUBMIT_DELAY_MS);
    await this.injectKeys(rt.lictorPort, "\r");

    rt.busy = true;
    return new Promise<string>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(payload.reqId);
        rt.busy = false;
        reject(new Error(`worker ${workerId} turn timeout (${this.cfg.turnTimeoutMs}ms)`));
      }, this.cfg.turnTimeoutMs);
      this.pending.set(payload.reqId, { workerId, resolve, reject, timer });
    });
  }

  /** callback 受信。対応する pending を resolve し、ワーカーを idle に戻す。 */
  onUtterance(reqId: string, _workerId: string, text: string): void {
    const p = this.pending.get(reqId);
    if (!p) {
      this.log.warn(`utterance for unknown/expired reqId ${reqId}`);
      return;
    }
    clearTimeout(p.timer);
    this.pending.delete(reqId);
    const rt = this.workers.get(p.workerId);
    if (rt) rt.busy = false;
    p.resolve(text);
  }

  /** Lictor sidecar の /v1/keys に raw キーを送る。 */
  private async injectKeys(port: number, data: string): Promise<void> {
    const res = await fetch(`http://127.0.0.1:${port}/v1/keys`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ data }),
    });
    if (!res.ok) {
      const t = await res.text().catch(() => "");
      throw new Error(`/v1/keys ${res.status}: ${t.slice(0, 120)}`);
    }
  }

  /** 全ワーカーを kill。 */
  stop(): void {
    for (const rt of this.workers.values()) {
      if (rt.settleTimer) clearTimeout(rt.settleTimer);
      killWorker(rt.pid, rt.lictorPid);
    }
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(new Error("pool stopped"));
    }
    this.pending.clear();
    this.workers.clear();
  }

  /** 診断用スナップショット。 */
  snapshot(): Array<{
    id: string;
    provider: string;
    model: string;
    port: number | null;
    settling: boolean;
    busy: boolean;
    pid: number | null;
    lictorPid: number | null;
  }> {
    return [...this.workers.values()].map((rt) => ({
      id: rt.config.id,
      provider: rt.config.provider,
      model: rt.config.model,
      port: rt.lictorPort,
      settling: rt.lictorPort != null && rt.readyAt == null,
      busy: rt.busy,
      pid: rt.pid,
      lictorPid: rt.lictorPid,
    }));
  }
}
