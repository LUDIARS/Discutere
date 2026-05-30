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

export type LlmBackend = "claude-cli" | "anthropic" | "mock";

export interface DiscutereConfig {
  nodeEnv: string;
  server: {
    port: number;
    frontendUrl: string;
  };
  /** 匿名議論 workspace (個人データ非保管) */
  workspace: string;
  discatier: {
    /** Discatier Core KG (Kuzu) のパス (未設定なら adapter 既定) */
    kuzuPath?: string;
  };
  personaEngine: {
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
  llm: {
    backend: LlmBackend;
    anthropicApiKey?: string;
    /** anthropic backend のモデル ID (未設定なら client 既定の haiku) */
    model?: string;
    /** claude-cli backend のタイムアウト ms */
    claudeCliTimeoutMs: number;
    /** Windows で claude CLI を spawn する際の git-bash パス (Memoria 方式) */
    gitBashPath?: string;
  };
  discord: {
    /** Gateway 接続用 bot token (未設定なら Gateway 起動を skip) */
    botToken?: string;
    /** slash command 登録用 application id (運用補助、本体動作には不要) */
    applicationId?: string;
    /** 単一ギルド運用時の guild id (運用補助) */
    guildId?: string;
    /** admin slash (kill/status) の認可 allowlist。空なら全 deny (安全 default) */
    adminIds: string[];
    /**
     * 平文メッセージ (非 slash) を utterance として取り込む議論チャンネル id。
     * 空なら取り込まない (安全 default — 無関係チャンネルのノイズ混入を防ぐ)。
     * slash command は本リストに依らず常に処理される。
     */
    discussionChannelIds: string[];
  };
}

interface RawFileConfig {
  server?: Partial<DiscutereConfig["server"]>;
  workspace?: string;
  discatier?: Partial<DiscutereConfig["discatier"]>;
  personaEngine?: Partial<DiscutereConfig["personaEngine"]>;
  llm?: Partial<DiscutereConfig["llm"]>;
  discord?: Partial<Omit<DiscutereConfig["discord"], "adminIds" | "discussionChannelIds">> & {
    adminIds?: string[];
    discussionChannelIds?: string[];
  };
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

export function loadConfig(): DiscutereConfig {
  const file = readFileConfig();
  const backendRaw = pick(process.env.LLM_BACKEND, file.llm?.backend, "anthropic").toLowerCase();
  const backend: LlmBackend =
    backendRaw === "claude-cli" || backendRaw === "mock" ? (backendRaw as LlmBackend) : "anthropic";

  return Object.freeze({
    nodeEnv: pick(process.env.NODE_ENV, undefined, "development"),
    server: {
      port: pickNum(process.env.BACKEND_PORT, file.server?.port, 3100),
      frontendUrl: pick(process.env.FRONTEND_URL, file.server?.frontendUrl, "http://localhost:5174"),
    },
    workspace: pick(process.env.DISCATIER_WORKSPACE, file.workspace, "knowledge"),
    discatier: {
      kuzuPath: pickOpt(process.env.DISCATIER_KUZU_PATH, file.discatier?.kuzuPath),
    },
    personaEngine: {
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
    llm: {
      backend,
      anthropicApiKey: pickOpt(process.env.ANTHROPIC_API_KEY, file.llm?.anthropicApiKey),
      model: pickOpt(process.env.ANTHROPIC_MODEL, file.llm?.model),
      claudeCliTimeoutMs: pickNum(process.env.CLAUDE_CLI_TIMEOUT_MS, file.llm?.claudeCliTimeoutMs, 120_000),
      gitBashPath: pickOpt(process.env.CLAUDE_CODE_GIT_BASH_PATH, file.llm?.gitBashPath),
    },
    discord: {
      botToken: pickOpt(process.env.DISCUTERE_DISCORD_BOT_TOKEN, file.discord?.botToken),
      applicationId: pickOpt(process.env.DISCUTERE_DISCORD_APPLICATION_ID, file.discord?.applicationId),
      guildId: pickOpt(process.env.DISCUTERE_DISCORD_GUILD_ID, file.discord?.guildId),
      adminIds: parseStringList(process.env.DISCUTERE_DISCORD_ADMIN_IDS, file.discord?.adminIds),
      discussionChannelIds: parseStringList(
        process.env.DISCUTERE_DISCORD_DISCUSSION_CHANNELS,
        file.discord?.discussionChannelIds
      ),
    },
  });
}

let cached: DiscutereConfig | null = null;

/** プロセス内で 1 度だけ load してキャッシュ (起動時に確定) */
export function getConfig(): DiscutereConfig {
  if (!cached) cached = loadConfig();
  return cached;
}
