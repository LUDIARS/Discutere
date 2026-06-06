/**
 * 常駐ワーカープール — 型定義。
 * spec/feature/persistent-worker-pool.md 参照。
 */

export type WorkerProvider = "claude" | "codex";

/** 1 ワーカー = 1 ペルソナ = 1 常駐 Lictor セッション。 */
export interface WorkerConfig {
  /** ワーカー / ペルソナ id (一意)。例 "con-opus" */
  id: string;
  /** 役割ラベル。例 "否定派" */
  role: string;
  /** Lictor がラップする CLI。 */
  provider: WorkerProvider;
  /** CLI に --model で渡すモデル ID。例 "claude-opus-4-8" / "gpt-5.5" */
  model: string;
}

/** WorkerPool の動作設定 (config.workerPool より構築)。 */
export interface WorkerPoolConfig {
  enabled: boolean;
  /** 議論 workspace。既存 knowledge と隔離する想定 ("debate")。 */
  workspace: string;
  /** ワーカーが register / utterance を返す Discutere の base URL。 */
  callbackBaseUrl: string;
  /** Windows で lictor → claude CLI を起動する際の git-bash パス (空なら自動検出)。 */
  gitBashPath?: string;
  /** auto-inject の TUI 待ち ms。 */
  injectDelayMs: number;
  /** 1 ターンの発話生成タイムアウト ms。 */
  turnTimeoutMs: number;
  /** spawn 後、ワーカーが自己 register するまでの待ち上限 ms。 */
  registerTimeoutMs: number;
  /** ターン JSON を書き出すディレクトリ。 */
  turnsDir: string;
  /** standing prompt md を書き出すディレクトリ。 */
  promptsDir: string;
  /** ワーカー定義。 */
  workers: WorkerConfig[];
}

/** pool が握る 1 ワーカーの実行時状態。 */
export interface WorkerRuntime {
  config: WorkerConfig;
  /** spawn した子プロセス (cmd) の pid。kill 用。 */
  pid: number | null;
  /** 自己 register で受領した Lictor sidecar port。未登録は null。 */
  lictorPort: number | null;
  /** 直近ターンが処理中か (二重 dispatch 防止)。 */
  busy: boolean;
  /** standing prompt md path。 */
  promptPath: string;
}
