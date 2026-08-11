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
  /** CLI に --model で渡すモデル ID。例 "claude-sonnet-5" / "gpt-5.6" */
  model: string;
}

/** WorkerPool の動作設定 (config.workerPool より構築)。 */
export interface WorkerPoolConfig {
  enabled: boolean;
  /** 議論 workspace。既存 knowledge と隔離する想定 ("debate")。 */
  workspace: string;
  /** ワーカーが register / utterance を返す Discutere の base URL。 */
  callbackBaseUrl: string;
  /**
   * 各ワーカー (lictor claude/codex) を起動する cwd。
   * Discutere 本体リポではなく専用ディレクトリ (worker-home) を指す。
   * ここに専用 `.claude/settings.json` (edit-mode + register/send スクリプトの
   * allow-list) と `scripts/{register,send}.mjs` を置く。
   */
  workerCwd: string;
  /** Windows で lictor → claude CLI を起動する際の git-bash パス (空なら自動検出)。 */
  gitBashPath?: string;
  /** auto-inject の TUI 待ち ms。 */
  injectDelayMs: number;
  /** 1 ターンの発話生成タイムアウト ms。 */
  turnTimeoutMs: number;
  /** spawn 後、ワーカーが自己 register するまでの待ち上限 ms。 */
  registerTimeoutMs: number;
  /**
   * register 受領後、dispatch を許可するまでの待ち ms。
   * register.mjs の POST が来た時点でワーカーの TUI はまだ「登録完了」を
   * 生成中なので、その完了 + idle 復帰を待つためのバッファ。
   */
  registerSettleMs: number;
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
  /** spawn した子プロセス (cmd) の pid。kill 用フォールバック。 */
  pid: number | null;
  /**
   * register.mjs が env から読んだ Lictor 自身の node プロセス PID。
   * Windows で ConPTY tree を確実に kill するために cmd.pid より優先する。
   * Lictor が LICTOR_PID を env に設定していない環境では null のまま。
   */
  lictorPid: number | null;
  /** 自己 register で受領した Lictor sidecar port。未登録は null。 */
  lictorPort: number | null;
  /**
   * settle 完了時刻 (epoch ms)。null = まだ settle 待ち。
   * register 受領後 registerSettleMs 経過するまで dispatch を拒否する。
   * register.mjs の POST 時点ではワーカーの TUI がまだ「登録完了」を
   * 生成中であり、その間に /v1/keys を注入するとキーが落ちる。
   */
  readyAt: number | null;
  /** readyAt を設定する setTimeout handle。stopWorker/stop でキャンセルする。 */
  settleTimer: NodeJS.Timeout | null;
  /** 直近ターンが処理中か (二重 dispatch 防止)。 */
  busy: boolean;
  /** standing prompt md path。 */
  promptPath: string;
}
