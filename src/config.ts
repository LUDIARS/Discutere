/**
 * Discutere サービス固有 config (config ファイル化).
 *
 * env 散在を単一 typed config に集約する。読み込み優先順は:
 *   default < config ファイル (JSON) < env
 * (= 平常運用は JSON、秘密情報/CI は env で上書き)。
 *
 * config ファイルパスは env `DISCUTERE_CONFIG` (既定 `./discutere.config.json`)。
 * 無ければ env / default のみで動く (= 後方互換、従来の .env 運用も生きる)。
 *
 * MACHINA (Chat-to-Task, Iv 移管対象) 系の env は本 config に含めない。
 */

import { existsSync, readFileSync } from "node:fs";
import path from "node:path";

export type LlmBackend = "claude-cli" | "anthropic" | "mock" | "worker-pool" | "local";

/** 常駐ワーカー 1 体 (= 1 ペルソナ) の定義。 */
export interface WorkerPoolWorker {
  id: string;
  role: string;
  provider: "claude" | "codex";
  model: string;
}

export interface DiscutereConfig {
  nodeEnv: string;
  server: {
    port: number;
    frontendUrl: string;
  };
  /** 4 フロー共通の進行量設定 */
  flow: {
    /** 最大ラウンド数 */
    rounds: number;
    /** 1 ラウンドあたりのターン数 */
    turnsPerRound: number;
    /** 議論プレイヤー人数 */
    personaCount: number;
    /** 投票者数 (中立) */
    voterCount: number;
    /** テーマ単位の既定タグ (`機密` `内部` `運用` `開発` など) */
    tags: string[];
    /** 壁打ちの暴走ガード上限 */
    sparringMaxTurns: number;
    /** 感情データ 0 件時の YouTube コメント取得上限 */
    youtubeMaxComments: number;
    /**
     * crawl (ext-ingest / ext-import) 完了後に C1 実在採用 (persona:adopt 相当) を
     * 自動実行するか (C1-b 自動採用フック)。既定 false (手動バッチ運用)。
     */
    autoAdoptOnCrawl: boolean;
    /**
     * 議論/改善フロー開始時、テーマの学習データが不足していたら指定ソースで
     * 学習クロール → KG 取込してから議論を始める (事前学習の UI 化)。
     * UI (`/flow`) は議論ごとにソース/パラメータを上書きでき、ここはその既定値。
     */
    autoCrawl: {
      /** 有効化。既定 true。 */
      enabled: boolean;
      /** 既定クロールソース。テーマ文字列だけで引ける niconico を既定 (API キー不要)。 */
      source: string;
      /** 学習データ充足とみなす外部の声の最小件数。これ未満ならクロールする。既定 3。 */
      minVoices: number;
      /** 1 回のクロールで取り込む最大件数。既定 200。 */
      maxItems: number;
    };
  };
  /** 匿名議論 workspace (個人データ非保管) */
  workspace: string;
  discatier: {
    /**
     * Discatier Core KG (Kuzu) のパス (未設定なら adapter 既定)。
     * 後方互換: knowledgeGraphs 未宣言時はこれが id="default" の単一 KG として扱われる。
     */
    kuzuPath?: string;
    /**
     * 起動時に開く KG の id (タスク別 KG レジストリの active 指定、 §12)。
     * 未設定なら "default"。 env `DISCATIER_ACTIVE_KG` で上書き可 (反映は再起動)。
     */
    activeKg?: string;
    /**
     * タスク (収集目的) 別 KG レジストリ (§12)。 config ファイルからのみ宣言する。
     * 宣言があれば activeKg の id に一致する kuzuPath を採用 (resolveActiveKgPath)。
     */
    knowledgeGraphs?: Array<{ id: string; label?: string; kuzuPath: string }>;
  };
  personaEngine: {
    /**
     * 旧 persona-engine (ルールエンジン司会 / facilitator / consensus-scorer / auto-seed) を
     * 起動するか。既定 false。議論は新フロー (src/flow) に集約済みで persona-engine は不使用
     * (OVERVIEW §4)。LLM クライアント (worker-pool) や peDb の構築は enabled に依らず維持する。
     */
    enabled: boolean;
    dbPath: string;
    /** 同一 session 内総発火上限 (turn budget) */
    maxFiresPerSession: number;
    /** 同一 session 内同一 rule 発火上限 (loop guard) */
    maxFiresPerRule: number;
    /** engine tick 周期 ms */
    tickMs: number;
    /** events table polling 周期 ms */
    bridgePollMs: number;
  };
  /** 議論ファシリテーター (停滞→拡張 / persona 過多→収束)。spec/facilitator/DESIGN.md */
  facilitator: {
    /** 有効化 (既定 true) */
    enabled: boolean;
    /** 見張り周期 ms (既定 30_000) */
    tickMs: number;
    /** 発言が無くなってから「停滞」とみなす空白 ms (既定 120_000) */
    idleGapMs: number;
    /** 参加 persona がこの数を超えたら強制収束 (安全上限、 既定 20) */
    maxPersonas: number;
    /** 止揚 (アウフヘーベン) がこの数たまったら収束 (既定 3) */
    aufhebungTarget: number;
  };
  /**
   * 自動シード議論 (#64/#65)。 種 (ジャンル / ストアトレンド) から headless 議論を
   * 定期的に立て、 facilitator 機構が収束まで回す。 学習データを自走で蓄積する。
   */
  autoSeed: {
    /** 有効化 (既定 false — opt-in) */
    enabled: boolean;
    /** シード投入の周期 ms (既定 1_800_000 = 30 分) */
    intervalMs: number;
    /** 同時に開いておく headless 議論の上限 (これ未満の時だけ新規シード、 既定 2) */
    maxConcurrent: number;
    /** 種ソース ("genre" | "store-trend")。 複数指定で巡回 (既定 ["genre"]) */
    sources: string[];
  };
  llm: {
    backend: LlmBackend;
    anthropicApiKey?: string;
    /** anthropic backend のモデル ID (未設定なら client 既定の haiku) */
    model?: string;
    /**
     * tier ルーティング用モデル (任意)。設定すると facilitator の重いタスク
     * (収束/止揚/視点追加) を strong、軽いタスクを cheap に振り分ける
     * (@ludiars/llm-gateway の pickTier)。未設定なら従来通り単一 model。
     */
    tiers?: {
      cheap?: string;
      strong?: string;
    };
    /** claude-cli backend のタイムアウト ms */
    claudeCliTimeoutMs: number;
    /** Windows で claude CLI を spawn する際の git-bash パス (Memoria 方式) */
    gitBashPath?: string;
    /**
     * local backend (OpenAI 互換ローカル LLM、 将来 Gemma 等)。 backend=local 時に使う。
     * Ollama/vLLM/LM Studio/llama.cpp server が公開する `/v1/chat/completions` を叩く。
     */
    local: {
      /** OpenAI 互換ベース URL (末尾 `/v1`)。 既定 Ollama `http://localhost:11434/v1`。 */
      baseUrl: string;
      /** モデル名 (例 `gemma4:12b`)。 */
      model: string;
      /** 任意。 設定時のみ Bearer 認証 (vLLM 等)。 Ollama は不要。 */
      apiKey?: string;
      /** 応答待ちタイムアウト ms (ローカルは遅いので長め)。 */
      timeoutMs: number;
    };
  };
  /**
   * 投稿→議題化の分類器 (classifyDiscordMessage) 専用 LLM 設定。
   * persona engine の worker-pool ルーティングとは独立させ、軽量モデル (Haiku)
   * を既定とする。Lictor 経由 (claude-cli) でも API 直 (anthropic) でも呼べる。
   * spec/feature/message-classifier.md。
   */
  classifier: {
    /**
     * "claude-cli" = Lictor 経由 spawn (サブスク・トークン不要) /
     * "anthropic" = API 直 / "local" = ローカル LLM (llm.local 設定を共用) /
     * "off" = LLM 無効 (regex fallback のみ)。
     */
    backend: "claude-cli" | "anthropic" | "local" | "off";
    /** 分類モデル ID (既定 Haiku)。精度優先なら Sonnet 等に切替可。 */
    model: string;
    /** claude-cli backend のタイムアウト ms (分類は短文なので短め) */
    timeoutMs: number;
  };
  /**
   * 議論パーティ編成 (DiscussionDirector)。想定発話数から人数を算出し、
   * 司会/キーマン(Opus 固定) + 意見(Sonnet:Haiku 重み抽選) を編成する。
   * 賛否はターン時に決定。予算到達でユーザに続行/停止を問い、続行時は半数入替。
   * spec/feature/discussion-party.md。
   */
  discussion: {
    /** 想定発話数。人数算出 (max(minTotal, ceil(n/4)+1)) と続行ゲートの基準。 */
    expectedUtterances: number;
    /** キーマン人数 (Opus 固定、入替後もモデル維持)。 */
    keymanCount: number;
    /** 司会のモデル。 */
    facilitatorModel: string;
    /** キーマンのモデル。 */
    keymanModel: string;
    /** 意見メンバーのモデル重み抽選表 (既定 Sonnet:Haiku = 6:4)。 */
    opinionModels: Array<{ model: string; weight: number }>;
    /** 総人数の下限。 */
    minTotal: number;
    /** ターン間ディレイ ms (Discord 投稿ペース)。 */
    turnDelayMs: number;
    /** 続行/停止ボタンの応答待ち ms (無応答は停止)。 */
    continueTimeoutMs: number;
    /** ラウンド上限 (暴走ガード)。 */
    maxRounds: number;
  };
  /**
   * 常駐ワーカープール (backend=worker-pool 時)。各ペルソナをサブスクの Lictor
   * セッションとして常駐させ、議論ターンを注入して発話を返させる。
   * spec/feature/persistent-worker-pool.md。
   */
  workerPool: {
    enabled: boolean;
    /** 議論 workspace (既存 knowledge と隔離する想定)。 */
    workspace: string;
    /** ワーカーが register / utterance を返す Discutere の base URL。 */
    callbackBaseUrl: string;
    /** Windows で lictor→CLI を起動する git-bash パス (空なら自動検出)。 */
    gitBashPath?: string;
    injectDelayMs: number;
    turnTimeoutMs: number;
    registerTimeoutMs: number;
    /** register 受領後 dispatch を解禁するまでの待ち ms (TUI idle 復帰バッファ)。 */
    registerSettleMs: number;
    /** ワーカー定義 (空なら index.ts が DEFAULT_WORKERS を使う)。 */
    workers: WorkerPoolWorker[];
    /**
     * 議論パーティから除外する provider (例 ["codex"] で GPT 系を全除外)。
     * トークンの無い provider を外して claude のみで回す、 等の用途。
     * 除外された persona の rule は seed されず、 既存 seed も boot で disabled になる。
     */
    excludeProviders: string[];
  };
  discord: {
    /** Gateway 接続用 bot token (未設定なら Gateway 起動を skip) */
    botToken?: string;
    /** slash command 登録用 application id (起動時自動登録に使用。未設定なら client.application.id を使う) */
    applicationId?: string;
    /**
     * 運用する guild id 群。複数サーバ運用ではここに全 guild を列挙する。
     * - slash command の即時登録 (guild commands) 先
     * - guild ごとの discutere-monitor 状態カード設置先
     * 空なら global command 登録 (反映に最大1時間) + monitor カードなしで動く。
     * すべて同一 workspace (= 単一 KG) に集約される。
     */
    guildIds: string[];
    /** admin slash (kill/status/backup) の認可 allowlist。空なら全 deny (安全 default) */
    adminIds: string[];
    /**
     * 平文メッセージ (非 slash) を utterance として取り込む議論チャンネル id。
     * 空なら取り込まない (安全 default — 無関係チャンネルのノイズ混入を防ぐ)。
     * slash command は本リストに依らず常に処理される。
     * スレッド内の発言は親チャンネルが本リストにあれば取り込む (自然な議論の継承)。
     */
    discussionChannelIds: string[];
    /**
     * 貼られた URL から外部議論データを取り込む「データクロール」 チャンネル id。
     * 空ならクロール無効 (安全 default)。 取り込み結果は「データ追加」 通知チャンネルへ。
     */
    crawlChannelIds: string[];
    /**
     * フォーラム集約: guild 内の Forum チャンネルを「議論カテゴリ」として監視する。
     * 各ポストの最初の投稿で議論を起こし、収束したらポストを archive+lock してクローズ、
     * まとめを「まとめ投稿」チャンネルへ転記する。データ学習依頼 / まとめ投稿チャンネルは
     * 起動時に自動作成する (Manage Channels 権限が必要)。
     */
    forum: {
      /** フォーラム監視を有効化 (既定 true)。 */
      enabled: boolean;
      /** 収束まとめの転記先チャンネル名 (既定「まとめ投稿」)。 */
      summaryChannelName: string;
      /** データクロール依頼の入口チャンネル名 (既定「データ学習依頼」)。 */
      dataLearningChannelName: string;
      /** 自動作成チャンネルの親カテゴリ名 (既定「システム」)。 */
      managedCategoryName: string;
      /**
       * 議論の方向性を決めるフォーラムタグ名 (部分一致)。
       * improvement 系タグ → 「改善提案」方向、fun 系 → 「面白さ」方向。
       * タグ無し / 未一致は既定で「面白さ」方向。
       */
      improvementTagNames: string[];
      funTagNames: string[];
      /** 起動時に ensure する議論フォーラム名 (既定「議論」)。定義済みフロータグを用意する。 */
      discussionForumName: string;
    };
    /**
     * ゲーム感想チャンネル。カテゴリ「ゲーム感想」配下のチャンネル (チャンネル名 =
     * ゲームタイトル) に投稿された感想を、議論にせず意見データとして匿名収集する。
     * カテゴリが無ければ起動時に作成する。spec/feature/game-feedback.md。
     */
    gameFeedback: {
      /** 有効化 (既定 true)。 */
      enabled: boolean;
      /** 感想チャンネルの親カテゴリ名 (既定「ゲーム感想」)。 */
      categoryName: string;
    };
  };
  /**
   * Slack Socket Mode トランスポート (Discord と並ぶ第2入口)。
   * 公開 URL 不要・常時 WS で議論/改善/学習/壁打ちをスレッド上で回す。
   * appToken/botToken/channelIds が揃い enabled の時のみ起動 (安全 default)。
   */
  slack: {
    /** Slack 連携を有効化 (既定 false)。 */
    enabled: boolean;
    /** app-level token (xapp-、Socket Mode)。空なら無効。 */
    appToken: string;
    /** bot token (xoxb-、Web API)。空なら無効。 */
    botToken: string;
    /** 議論を起こす Slack channel id。空なら起動しない (安全 default)。 */
    channelIds: string[];
    /** 将来の管理コマンド用 allowlist (現状は保持のみ)。 */
    adminIds: string[];
  };
  /**
   * 学習データ (Discatier KG + persona-engine.db + discutere.db) の S3 アーカイブ。
   * tar.gz 化して S3 (Glacier 系ストレージクラス想定) に push する。月次自動 + 手動。
   */
  backup: {
    /** 自動バックアップを有効化するか (手動 slash/script は enabled に依らず常に可) */
    enabled: boolean;
    /** S3 bucket 名 (未設定ならバックアップ不可) */
    bucket?: string;
    /** S3 region */
    region: string;
    /** key prefix (例 "discutere/") */
    prefix: string;
    /** ストレージクラス (GLACIER / DEEP_ARCHIVE / STANDARD_IA / STANDARD ...) */
    storageClass: string;
    /** S3 互換エンドポイント (MinIO 等。未設定なら AWS) */
    endpoint?: string;
    /** 認証情報 (未設定なら AWS SDK の既定チェーン = 環境/IAM ロール) */
    accessKeyId?: string;
    secretAccessKey?: string;
    /** 自動バックアップ周期 (日)。既定 30 = 月次 */
    intervalDays: number;
    /** 最終実行時刻を記録する state ファイル */
    stateFile: string;
  };
  /** KG 共有同期 (spec/core/KG-SYNC.md)。follower が master の共有知識を pull する。 */
  kgSync: {
    /** follower が差分を取得する URL (master endpoint or 静的 publish)。未設定なら pull 無効。 */
    sourceUrl?: string;
    /** 配信元が要求する共有シークレット (master の serveToken と一致させる。任意)。 */
    token?: string;
    /** 起動時 + 定期 pull を有効化するか (sourceUrl も必要)。 */
    auto: boolean;
    /** 定期 pull の周期 (時間)。既定 6。 */
    intervalHours: number;
    /** follower の同期状態 (透かし) ファイル。 */
    stateFile: string;
    /** master の /api/kg/migrations が要求する共有シークレット (未設定なら誰でも取得可)。 */
    serveToken?: string;
  };
  /**
   * LLM コストの横断集約 (Anatomia コスト削減UIへの relay)。
   * llm_call_log のセッション別集計を Anatomia の POST /api/cost-feed へ PUSH する。
   * 既定 OFF。`anatomiaUrl` + `enabled` が揃った時のみ起動時に定期 relay する。
   */
  cost: {
    relay: {
      /** 定期 relay を有効化するか (anatomiaUrl も必要)。既定 false。 */
      enabled: boolean;
      /** Anatomia のベース URL (例 http://localhost:4200)。未設定なら relay しない。 */
      anatomiaUrl?: string;
      /** 定期 PUSH 周期 ms。既定 300_000 (5分)。 */
      intervalMs: number;
      /** cost-feed の service ラベル。既定 "discutere"。 */
      service: string;
    };
  };
}

interface RawFileConfig {
  server?: Partial<DiscutereConfig["server"]>;
  flow?: Partial<DiscutereConfig["flow"]>;
  workspace?: string;
  discatier?: Partial<DiscutereConfig["discatier"]>;
  personaEngine?: Partial<DiscutereConfig["personaEngine"]>;
  facilitator?: Partial<DiscutereConfig["facilitator"]>;
  autoSeed?: Partial<DiscutereConfig["autoSeed"]>;
  llm?: Partial<DiscutereConfig["llm"]>;
  classifier?: Partial<DiscutereConfig["classifier"]>;
  discussion?: Partial<DiscutereConfig["discussion"]>;
  workerPool?: Partial<Omit<DiscutereConfig["workerPool"], "workers">> & { workers?: WorkerPoolWorker[] };
  discord?: Partial<
    Omit<
      DiscutereConfig["discord"],
      "adminIds" | "discussionChannelIds" | "crawlChannelIds" | "guildIds" | "forum" | "gameFeedback"
    >
  > & {
    adminIds?: string[];
    discussionChannelIds?: string[];
    crawlChannelIds?: string[];
    guildIds?: string[];
    forum?: Partial<DiscutereConfig["discord"]["forum"]>;
    gameFeedback?: Partial<DiscutereConfig["discord"]["gameFeedback"]>;
    /** 後方互換: 旧 単数 guildId (guildIds に統合される) */
    guildId?: string;
  };
  slack?: Partial<Omit<DiscutereConfig["slack"], "channelIds" | "adminIds">> & {
    channelIds?: string[];
    adminIds?: string[];
  };
  backup?: Partial<DiscutereConfig["backup"]>;
  kgSync?: Partial<DiscutereConfig["kgSync"]>;
  cost?: { relay?: Partial<DiscutereConfig["cost"]["relay"]> };
}

function readFileConfig(): RawFileConfig {
  const file = process.env.DISCUTERE_CONFIG ?? "./discutere.config.json";
  const abs = path.resolve(file);
  if (!existsSync(abs)) return {};
  try {
    const parsed = JSON.parse(readFileSync(abs, "utf8")) as RawFileConfig;
    return parsed && typeof parsed === "object" ? parsed : {};
  } catch (err) {
    console.warn(`[config] failed to parse ${abs}: ${(err as Error).message} — ignoring file`);
    return {};
  }
}

/** env 優先 → file → default の順で最初に「定義された」値を採る (string) */
function pick(envValue: string | undefined, fileValue: unknown, dflt: string): string {
  if (envValue !== undefined && envValue !== "") return envValue;
  if (typeof fileValue === "string" && fileValue !== "") return fileValue;
  return dflt;
}

/** 同上 (optional: 定義が無ければ undefined) */
function pickOpt(envValue: string | undefined, fileValue: unknown): string | undefined {
  if (envValue !== undefined && envValue !== "") return envValue;
  if (typeof fileValue === "string" && fileValue !== "") return fileValue;
  return undefined;
}

/** boolean 版。env("1"/"true"/"yes" → true) → file(boolean) → default */
function pickBool(envValue: string | undefined, fileValue: unknown, dflt: boolean): boolean {
  if (envValue !== undefined && envValue !== "") {
    return /^(1|true|yes|on)$/i.test(envValue.trim());
  }
  if (typeof fileValue === "boolean") return fileValue;
  return dflt;
}

/** number 版。env(string)→file(number|string)→default */
function pickNum(envValue: string | undefined, fileValue: unknown, dflt: number): number {
  if (envValue !== undefined && envValue !== "") {
    const n = Number(envValue);
    if (Number.isFinite(n)) return n;
  }
  if (typeof fileValue === "number" && Number.isFinite(fileValue)) return fileValue;
  if (typeof fileValue === "string" && fileValue !== "") {
    const n = Number(fileValue);
    if (Number.isFinite(n)) return n;
  }
  return dflt;
}

/** カンマ区切り env → 無ければ file の string 配列 → 無ければ空。両者の最初に存在した方を採用 */
function parseStringList(env: string | undefined, fileValue: unknown): string[] {
  const fromEnv = (env ?? "")
    .split(",")
    .map((s) => s.trim())
    .filter((s) => s.length > 0);
  if (fromEnv.length > 0) return fromEnv;
  if (Array.isArray(fileValue)) {
    return fileValue.filter((s): s is string => typeof s === "string" && s.trim().length > 0).map((s) => s.trim());
  }
  return [];
}

/** 複数 guildIds と後方互換の単数 guildId を結合・重複排除する */
function mergeGuildIds(list: string[], single: string | undefined): string[] {
  const set = new Set(list);
  if (single) set.add(single);
  return [...set];
}

/** env (カンマ区切り) → file 配列 の順で読む文字列リスト。 trim + 空除外。 */
function parseCsvList(envValue: string | undefined, fileValue: unknown): string[] {
  if (typeof envValue === "string" && envValue.trim().length > 0) {
    return envValue.split(",").map((s) => s.trim()).filter((s) => s.length > 0);
  }
  if (Array.isArray(fileValue)) {
    return fileValue.filter((s): s is string => typeof s === "string" && s.trim().length > 0).map((s) => s.trim());
  }
  return [];
}

export function loadConfig(): DiscutereConfig {
  const file = readFileConfig();
  const backendRaw = pick(process.env.LLM_BACKEND, file.llm?.backend, "anthropic").toLowerCase();
  const backend: LlmBackend =
    backendRaw === "claude-cli" ||
    backendRaw === "mock" ||
    backendRaw === "worker-pool" ||
    backendRaw === "local"
      ? (backendRaw as LlmBackend)
      : "anthropic";

  // 分類器 backend: 既定は Lictor 経由 (claude-cli)。ANTHROPIC_API_KEY のみ存在する
  // 環境では API 直 (anthropic) に倒す (CLI が無い CI/headless での後方互換)。
  const classifierBackendRaw = pick(
    process.env.DISCUTERE_CLASSIFIER_BACKEND,
    file.classifier?.backend,
    process.env.ANTHROPIC_API_KEY ? "anthropic" : "claude-cli"
  ).toLowerCase();
  const classifierBackend: DiscutereConfig["classifier"]["backend"] =
    classifierBackendRaw === "anthropic" ||
    classifierBackendRaw === "off" ||
    classifierBackendRaw === "local"
      ? (classifierBackendRaw as DiscutereConfig["classifier"]["backend"])
      : "claude-cli";

  return Object.freeze({
    nodeEnv: pick(process.env.NODE_ENV, undefined, "development"),
    server: {
      port: pickNum(process.env.BACKEND_PORT, file.server?.port, 3100),
      frontendUrl: pick(process.env.FRONTEND_URL, file.server?.frontendUrl, "http://localhost:5174"),
    },
    flow: {
      rounds: pickNum(process.env.DISCUTERE_FLOW_ROUNDS, file.flow?.rounds, 3),
      turnsPerRound: pickNum(process.env.DISCUTERE_FLOW_TURNS_PER_ROUND, file.flow?.turnsPerRound, 6),
      personaCount: pickNum(process.env.DISCUTERE_FLOW_PERSONA_COUNT, file.flow?.personaCount, 4),
      voterCount: pickNum(process.env.DISCUTERE_FLOW_VOTER_COUNT, file.flow?.voterCount, 3),
      tags: parseStringList(process.env.DISCUTERE_FLOW_TAGS, file.flow?.tags),
      sparringMaxTurns: pickNum(process.env.DISCUTERE_FLOW_SPARRING_MAX_TURNS, file.flow?.sparringMaxTurns, 50),
      youtubeMaxComments: pickNum(process.env.DISCUTERE_FLOW_YOUTUBE_MAX_COMMENTS, file.flow?.youtubeMaxComments, 200),
      autoAdoptOnCrawl: pickBool(
        process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL,
        file.flow?.autoAdoptOnCrawl,
        false
      ),
      autoCrawl: {
        enabled: pickBool(
          process.env.DISCUTERE_FLOW_AUTOCRAWL_ENABLED,
          file.flow?.autoCrawl?.enabled,
          true
        ),
        source: pick(
          process.env.DISCUTERE_FLOW_AUTOCRAWL_SOURCE,
          file.flow?.autoCrawl?.source,
          "niconico"
        ),
        minVoices: pickNum(
          process.env.DISCUTERE_FLOW_AUTOCRAWL_MIN_VOICES,
          file.flow?.autoCrawl?.minVoices,
          3
        ),
        maxItems: pickNum(
          process.env.DISCUTERE_FLOW_AUTOCRAWL_MAX_ITEMS,
          file.flow?.autoCrawl?.maxItems,
          200
        ),
      },
    },
    workspace: pick(process.env.DISCATIER_WORKSPACE, file.workspace, "knowledge"),
    discatier: {
      kuzuPath: pickOpt(process.env.DISCATIER_KUZU_PATH, file.discatier?.kuzuPath),
      activeKg: pickOpt(process.env.DISCATIER_ACTIVE_KG, file.discatier?.activeKg),
      // knowledgeGraphs は config ファイルからのみ (env で配列は受けない)。不正要素は除外。
      knowledgeGraphs: Array.isArray(file.discatier?.knowledgeGraphs)
        ? file.discatier!.knowledgeGraphs.filter(
            (kg): kg is { id: string; label?: string; kuzuPath: string } =>
              !!kg && typeof kg.id === "string" && typeof kg.kuzuPath === "string"
          )
        : undefined,
    },
    personaEngine: {
      enabled: pickBool(
        process.env.DISCUTERE_PERSONA_ENGINE_ENABLED,
        file.personaEngine?.enabled,
        false
      ),
      dbPath: pick(
        process.env.DISCUTERE_PERSONA_ENGINE_DB,
        file.personaEngine?.dbPath,
        path.resolve("./data/persona-engine.db")
      ),
      maxFiresPerSession: pickNum(
        process.env.PERSONA_ENGINE_MAX_FIRES_PER_SESSION,
        file.personaEngine?.maxFiresPerSession,
        20
      ),
      maxFiresPerRule: pickNum(
        process.env.PERSONA_ENGINE_MAX_FIRES_PER_RULE,
        file.personaEngine?.maxFiresPerRule,
        5
      ),
      tickMs: pickNum(process.env.PERSONA_ENGINE_TICK_MS, file.personaEngine?.tickMs, 5000),
      bridgePollMs: pickNum(
        process.env.PERSONA_ENGINE_BRIDGE_POLL_MS,
        file.personaEngine?.bridgePollMs,
        2000
      ),
    },
    facilitator: {
      enabled: pickBool(process.env.DISCUTERE_FACILITATOR_ENABLED, file.facilitator?.enabled, true),
      tickMs: pickNum(process.env.DISCUTERE_FACILITATOR_TICK_MS, file.facilitator?.tickMs, 30_000),
      idleGapMs: pickNum(process.env.DISCUTERE_FACILITATOR_IDLE_GAP_MS, file.facilitator?.idleGapMs, 120_000),
      maxPersonas: pickNum(process.env.DISCUTERE_FACILITATOR_MAX_PERSONAS, file.facilitator?.maxPersonas, 20),
      aufhebungTarget: pickNum(process.env.DISCUTERE_FACILITATOR_AUFHEBUNG_TARGET, file.facilitator?.aufhebungTarget, 3),
    },
    autoSeed: {
      enabled: pickBool(process.env.DISCUTERE_AUTOSEED_ENABLED, file.autoSeed?.enabled, false),
      intervalMs: pickNum(process.env.DISCUTERE_AUTOSEED_INTERVAL_MS, file.autoSeed?.intervalMs, 1_800_000),
      maxConcurrent: pickNum(process.env.DISCUTERE_AUTOSEED_MAX_CONCURRENT, file.autoSeed?.maxConcurrent, 2),
      sources: parseStringList(process.env.DISCUTERE_AUTOSEED_SOURCES, file.autoSeed?.sources).length
        ? parseStringList(process.env.DISCUTERE_AUTOSEED_SOURCES, file.autoSeed?.sources)
        : ["genre"],
    },
    llm: {
      backend,
      anthropicApiKey: pickOpt(process.env.ANTHROPIC_API_KEY, file.llm?.anthropicApiKey),
      model: pickOpt(process.env.ANTHROPIC_MODEL, file.llm?.model),
      tiers: {
        cheap: pickOpt(process.env.LLM_TIER_CHEAP, file.llm?.tiers?.cheap),
        strong: pickOpt(process.env.LLM_TIER_STRONG, file.llm?.tiers?.strong),
      },
      claudeCliTimeoutMs: pickNum(process.env.CLAUDE_CLI_TIMEOUT_MS, file.llm?.claudeCliTimeoutMs, 120_000),
      gitBashPath: pickOpt(process.env.CLAUDE_CODE_GIT_BASH_PATH, file.llm?.gitBashPath),
      local: {
        // 既定は Ollama の OpenAI 互換エンドポイント。vLLM/LM Studio は baseUrl を差し替え。
        baseUrl: pick(process.env.LLM_LOCAL_BASE_URL, file.llm?.local?.baseUrl, "http://localhost:11434/v1"),
        model: pick(process.env.LLM_LOCAL_MODEL, file.llm?.local?.model, "gemma4:12b"),
        apiKey: pickOpt(process.env.LLM_LOCAL_API_KEY, file.llm?.local?.apiKey),
        timeoutMs: pickNum(process.env.LLM_LOCAL_TIMEOUT_MS, file.llm?.local?.timeoutMs, 120_000),
      },
    },
    classifier: {
      backend: classifierBackend,
      model: pick(
        process.env.DISCUTERE_CLASSIFIER_MODEL,
        file.classifier?.model,
        "claude-haiku-4-5-20251001"
      ),
      timeoutMs: pickNum(process.env.DISCUTERE_CLASSIFIER_TIMEOUT_MS, file.classifier?.timeoutMs, 30_000),
    },
    discussion: {
      expectedUtterances: pickNum(process.env.DISCUTERE_DISCUSSION_EXPECTED, file.discussion?.expectedUtterances, 20),
      keymanCount: pickNum(process.env.DISCUTERE_DISCUSSION_KEYMEN, file.discussion?.keymanCount, 2),
      facilitatorModel: pick(
        process.env.DISCUTERE_DISCUSSION_FACILITATOR_MODEL,
        file.discussion?.facilitatorModel,
        "claude-opus-4-8"
      ),
      keymanModel: pick(process.env.DISCUTERE_DISCUSSION_KEYMAN_MODEL, file.discussion?.keymanModel, "claude-opus-4-8"),
      opinionModels:
        Array.isArray(file.discussion?.opinionModels) && file.discussion!.opinionModels.length > 0
          ? file.discussion!.opinionModels
          : [
              { model: "claude-sonnet-4-6", weight: 6 },
              { model: "claude-haiku-4-5-20251001", weight: 4 },
            ],
      minTotal: pickNum(process.env.DISCUTERE_DISCUSSION_MIN_TOTAL, file.discussion?.minTotal, 4),
      turnDelayMs: pickNum(process.env.DISCUTERE_DISCUSSION_TURN_DELAY_MS, file.discussion?.turnDelayMs, 4_000),
      continueTimeoutMs: pickNum(
        process.env.DISCUTERE_DISCUSSION_CONTINUE_TIMEOUT_MS,
        file.discussion?.continueTimeoutMs,
        120_000
      ),
      maxRounds: pickNum(process.env.DISCUTERE_DISCUSSION_MAX_ROUNDS, file.discussion?.maxRounds, 5),
    },
    workerPool: {
      enabled: pickBool(process.env.DISCUTERE_WORKER_POOL_ENABLED, file.workerPool?.enabled, false),
      workspace: pick(process.env.DISCUTERE_WORKER_POOL_WORKSPACE, file.workerPool?.workspace, "debate"),
      callbackBaseUrl: pick(
        process.env.DISCUTERE_WORKER_POOL_CALLBACK_URL,
        file.workerPool?.callbackBaseUrl,
        `http://127.0.0.1:${pickNum(process.env.BACKEND_PORT, file.server?.port, 3100)}`
      ),
      gitBashPath: pickOpt(
        process.env.DISCUTERE_WORKER_POOL_GIT_BASH ?? process.env.CLAUDE_CODE_GIT_BASH_PATH,
        file.workerPool?.gitBashPath ?? file.llm?.gitBashPath
      ),
      injectDelayMs: pickNum(process.env.DISCUTERE_WORKER_POOL_INJECT_DELAY_MS, file.workerPool?.injectDelayMs, 2500),
      turnTimeoutMs: pickNum(process.env.DISCUTERE_WORKER_POOL_TURN_TIMEOUT_MS, file.workerPool?.turnTimeoutMs, 120_000),
      registerTimeoutMs: pickNum(
        process.env.DISCUTERE_WORKER_POOL_REGISTER_TIMEOUT_MS,
        file.workerPool?.registerTimeoutMs,
        60_000
      ),
      registerSettleMs: pickNum(
        process.env.DISCUTERE_WORKER_POOL_REGISTER_SETTLE_MS,
        file.workerPool?.registerSettleMs,
        4_000
      ),
      workers: Array.isArray(file.workerPool?.workers) ? file.workerPool!.workers : [],
      excludeProviders: parseCsvList(
        process.env.DISCUTERE_WORKER_POOL_EXCLUDE_PROVIDERS,
        file.workerPool?.excludeProviders
      ),
    },
    discord: {
      botToken: pickOpt(process.env.DISCUTERE_DISCORD_BOT_TOKEN, file.discord?.botToken),
      applicationId: pickOpt(process.env.DISCUTERE_DISCORD_APPLICATION_ID, file.discord?.applicationId),
      // guildIds: env (カンマ区切り) → file.guildIds → 後方互換で単数 guildId を1要素に。
      guildIds: mergeGuildIds(
        parseStringList(process.env.DISCUTERE_DISCORD_GUILD_IDS, file.discord?.guildIds),
        pickOpt(process.env.DISCUTERE_DISCORD_GUILD_ID, file.discord?.guildId)
      ),
      adminIds: parseStringList(process.env.DISCUTERE_DISCORD_ADMIN_IDS, file.discord?.adminIds),
      discussionChannelIds: parseStringList(
        process.env.DISCUTERE_DISCORD_DISCUSSION_CHANNELS,
        file.discord?.discussionChannelIds
      ),
      crawlChannelIds: parseStringList(
        process.env.DISCUTERE_DISCORD_CRAWL_CHANNELS,
        file.discord?.crawlChannelIds
      ),
      forum: {
        enabled: pickBool(process.env.DISCUTERE_DISCORD_FORUM_ENABLED, file.discord?.forum?.enabled, true),
        summaryChannelName: pick(
          process.env.DISCUTERE_DISCORD_FORUM_SUMMARY_CHANNEL,
          file.discord?.forum?.summaryChannelName,
          "まとめ投稿"
        ),
        dataLearningChannelName: pick(
          process.env.DISCUTERE_DISCORD_FORUM_DATA_CHANNEL,
          file.discord?.forum?.dataLearningChannelName,
          "データ学習依頼"
        ),
        managedCategoryName: pick(
          process.env.DISCUTERE_DISCORD_FORUM_CATEGORY,
          file.discord?.forum?.managedCategoryName,
          "システム"
        ),
        improvementTagNames: parseStringList(
          process.env.DISCUTERE_DISCORD_FORUM_IMPROVEMENT_TAGS,
          file.discord?.forum?.improvementTagNames
        ).length
          ? parseStringList(
              process.env.DISCUTERE_DISCORD_FORUM_IMPROVEMENT_TAGS,
              file.discord?.forum?.improvementTagNames
            )
          : ["改善提案", "改善", "提案", "improvement"],
        funTagNames: parseStringList(
          process.env.DISCUTERE_DISCORD_FORUM_FUN_TAGS,
          file.discord?.forum?.funTagNames
        ).length
          ? parseStringList(process.env.DISCUTERE_DISCORD_FORUM_FUN_TAGS, file.discord?.forum?.funTagNames)
          : ["面白さ", "面白い", "おもしろさ", "fun"],
        discussionForumName: pick(
          process.env.DISCUTERE_DISCORD_FORUM_NAME,
          file.discord?.forum?.discussionForumName,
          "議論"
        ),
      },
      gameFeedback: {
        enabled: pickBool(process.env.DISCUTERE_DISCORD_GAME_FEEDBACK_ENABLED, file.discord?.gameFeedback?.enabled, true),
        categoryName: pick(
          process.env.DISCUTERE_DISCORD_GAME_FEEDBACK_CATEGORY,
          file.discord?.gameFeedback?.categoryName,
          "ゲーム感想"
        ),
      },
    },
    slack: {
      enabled: pickBool(process.env.DISCUTERE_SLACK_ENABLED, file.slack?.enabled, false),
      appToken: pickOpt(process.env.DISCUTERE_SLACK_APP_TOKEN, file.slack?.appToken) ?? "",
      botToken: pickOpt(process.env.DISCUTERE_SLACK_BOT_TOKEN, file.slack?.botToken) ?? "",
      channelIds: parseStringList(process.env.DISCUTERE_SLACK_CHANNEL_IDS, file.slack?.channelIds),
      adminIds: parseStringList(process.env.DISCUTERE_SLACK_ADMIN_IDS, file.slack?.adminIds),
    },
    backup: {
      enabled: pickBool(process.env.DISCUTERE_BACKUP_ENABLED, file.backup?.enabled, false),
      bucket: pickOpt(process.env.DISCUTERE_BACKUP_BUCKET, file.backup?.bucket),
      region: pick(process.env.DISCUTERE_BACKUP_REGION ?? process.env.AWS_REGION, file.backup?.region, "ap-northeast-1"),
      prefix: pick(process.env.DISCUTERE_BACKUP_PREFIX, file.backup?.prefix, "discutere/"),
      storageClass: pick(process.env.DISCUTERE_BACKUP_STORAGE_CLASS, file.backup?.storageClass, "GLACIER"),
      endpoint: pickOpt(process.env.DISCUTERE_BACKUP_ENDPOINT, file.backup?.endpoint),
      accessKeyId: pickOpt(process.env.AWS_ACCESS_KEY_ID, file.backup?.accessKeyId),
      secretAccessKey: pickOpt(process.env.AWS_SECRET_ACCESS_KEY, file.backup?.secretAccessKey),
      intervalDays: pickNum(process.env.DISCUTERE_BACKUP_INTERVAL_DAYS, file.backup?.intervalDays, 30),
      stateFile: pick(
        process.env.DISCUTERE_BACKUP_STATE_FILE,
        file.backup?.stateFile,
        path.resolve("./data/backup-state.json")
      ),
    },
    kgSync: {
      sourceUrl: pickOpt(process.env.DISCUTERE_KG_SYNC_URL, file.kgSync?.sourceUrl),
      token: pickOpt(process.env.DISCUTERE_KG_SYNC_TOKEN, file.kgSync?.token),
      auto: pickBool(process.env.DISCUTERE_KG_SYNC_AUTO, file.kgSync?.auto, false),
      intervalHours: pickNum(process.env.DISCUTERE_KG_SYNC_INTERVAL_HOURS, file.kgSync?.intervalHours, 6),
      stateFile: pick(
        process.env.DISCUTERE_KG_SYNC_STATE_FILE,
        file.kgSync?.stateFile,
        path.resolve("./data/kg-sync-state.json")
      ),
      serveToken: pickOpt(process.env.DISCUTERE_KG_SYNC_SERVE_TOKEN, file.kgSync?.serveToken),
    },
    cost: {
      relay: {
        enabled: pickBool(process.env.DISCUTERE_COST_RELAY_ENABLED, file.cost?.relay?.enabled, false),
        anatomiaUrl: pickOpt(process.env.DISCUTERE_COST_RELAY_URL, file.cost?.relay?.anatomiaUrl),
        intervalMs: pickNum(
          process.env.DISCUTERE_COST_RELAY_INTERVAL_MS,
          file.cost?.relay?.intervalMs,
          300_000
        ),
        service: pick(process.env.DISCUTERE_COST_RELAY_SERVICE, file.cost?.relay?.service, "discutere"),
      },
    },
  });
}

let cached: DiscutereConfig | null = null;

/** プロセス内で 1 度だけ load してキャッシュ (起動時に確定) */
export function getConfig(): DiscutereConfig {
  if (!cached) cached = loadConfig();
  return cached;
}

/** テスト専用: キャッシュをクリアして次回 getConfig() で再ロードさせる */
export function _resetConfig(): void {
  cached = null;
}
