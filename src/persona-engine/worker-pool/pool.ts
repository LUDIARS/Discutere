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
import type { WorkerPoolConfig, WorkerRuntime } from "./types.js";

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

  /** 全ワーカーを spawn する。port は各ワーカーの自己 register で後から埋まる。 */
  start(): void {
    mkdirSync(this.absTurnsDir, { recursive: true });
    mkdirSync(this.absPromptsDir, { recursive: true });
    for (const worker of this.cfg.workers) {
      const promptPath = join(this.absPromptsDir, `${worker.id}.md`);
      const body = buildStandingPrompt({
        worker,
        callbackBaseUrl: this.cfg.callbackBaseUrl,
        turnsDir: this.cfg.turnsDir,
      });
      writeFileSync(promptPath, body, "utf8");
      const { pid } = spawnWorker({ worker, promptPath, cwd: this.cwd, cfg: this.cfg });
      this.workers.set(worker.id, {
        config: worker,
        pid,
        lictorPort: null,
        busy: false,
        promptPath,
      });
      this.log.info(`spawned worker ${worker.id} (${worker.provider}/${worker.model}) pid=${pid}`);
    }
  }

  hasWorker(id: string): boolean {
    return this.workers.has(id);
  }

  isReady(id: string): boolean {
    return this.workers.get(id)?.lictorPort != null;
  }

  /** ワーカーの自己 register。port を記録。 */
  registerPort(workerId: string, lictorPort: number): boolean {
    const rt = this.workers.get(workerId);
    if (!rt) {
      this.log.warn(`register from unknown worker ${workerId}`);
      return false;
    }
    rt.lictorPort = lictorPort;
    this.log.info(`worker ${workerId} registered port=${lictorPort}`);
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
    if (rt.busy) throw new Error(`worker ${workerId} busy`);

    const turnPath = join(this.absTurnsDir, `${payload.reqId}.json`);
    writeFileSync(
      turnPath,
      JSON.stringify({ reqId: payload.reqId, workerId, system: payload.system, prompt: payload.prompt }, null, 2),
      "utf8"
    );

    // 注入は relative path で (worker の cwd = Discutere)。1 行 + Enter。
    const line = `[TURN] ${payload.reqId} ${this.cfg.turnsDir}/${payload.reqId}.json\r`;
    await this.injectKeys(rt.lictorPort, line);

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
    for (const rt of this.workers.values()) killWorker(rt.pid);
    for (const p of this.pending.values()) {
      clearTimeout(p.timer);
      p.reject(new Error("pool stopped"));
    }
    this.pending.clear();
    this.workers.clear();
  }

  /** 診断用スナップショット。 */
  snapshot(): Array<{ id: string; provider: string; model: string; port: number | null; busy: boolean; pid: number | null }> {
    return [...this.workers.values()].map((rt) => ({
      id: rt.config.id,
      provider: rt.config.provider,
      model: rt.config.model,
      port: rt.lictorPort,
      busy: rt.busy,
      pid: rt.pid,
    }));
  }
}
