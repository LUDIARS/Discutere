import { join } from "node:path";

import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { cors } from "hono/cors";
import Database from "better-sqlite3";
import { machinaRoutes } from "./machina/routes.js";
import { userContext } from "./middleware/auth.js";
import { adminRoutes, setPersonaEngine, getPersonaEngine, setFacilitator, setGatewaySweeper } from "./api/admin-routes.js";
import { dashboardRoutes } from "./api/dashboard-routes.js";
import { errorsRoutes } from "./api/errors-routes.js";
import { consensusRoutes } from "./api/consensus-routes.js";
import { installConsoleCapture } from "./observability/console-capture.js";
import { createConsensusScorer, type ConsensusScorer } from "./persona-engine/scoring/consensus-scorer.js";
import { learningViewRoutes } from "./api/learning-view-routes.js";
import { startSessionCleanup } from "./machina/mode-state.js";
import { createCore } from "./core/index.js";
import { resolveActiveKgPath } from "./core/kg-registry.js";
import { createDiscatierContextProvider } from "./discatier-engine-adapter/index.js";
import { createEventBridge } from "./discatier-engine-adapter/event-bridge.js";
import {
  AnthropicSdkClient,
  ClaudeCliClient,
  LocalOpenAiClient,
  createPersonaEngine,
  type LLMClient,
  type PersonaSeed,
} from "./persona-engine/index.js";
import { readClaudeCodeToken } from "./persona-engine/llm/claude-code-auth.js";
import { WorkerPool } from "./persona-engine/worker-pool/pool.js";
import { WorkerPoolClient } from "./persona-engine/worker-pool/client.js";
import { DEFAULT_WORKERS, buildWorkerPersonaSeeds } from "./persona-engine/worker-pool/persona-prompts.js";
import { DEBATE_RULE_SEEDS } from "./persona-engine/worker-pool/debate-rules.js";
import { workerRoutes, setWorkerPool } from "./api/worker.js";
import { workerPoolControlRoutes, setWorkerPoolControl } from "./api/worker-pool-control.js";
import { tuningRoutes, setRuntimeSettings } from "./api/tuning-routes.js";
import { topPageRoutes } from "./api/top-page-routes.js";
import { webChatRoutes, setWebChatDeps } from "./api/web-chat-routes.js";
import { flowRoutes, setFlowWebDeps } from "./flow/web/routes.js";
import { runAdoptFromKg } from "./flow/persona-adopt-runner.js";
import { FallbackLlm } from "./flow/llm-fallback.js";
import { createGuideRoutes } from "./api/guide-routes.js";
import { createRuntimeSettingsStore } from "./runtime-settings/store.js";
import { setRolePromptResolver, ROLE_GUIDANCE_DEFAULTS } from "./persona-engine/worker-pool/persona-prompts.js";
import { applyRuleInstructionOverrides, RULE_INSTRUCTION_DEFAULTS } from "./persona-engine/worker-pool/debate-rules.js";
import { PersonasRepo } from "./persona-engine/db/personas-repo.js";
import { createFacilitator } from "./persona-engine/facilitator/index.js";
import { createAutoSeedScheduler } from "./discussion-seed/scheduler.js";
import { postDiscussionToDiscord } from "./discord-hook/discussion-bridge.js";
import { createDiscordAutoDiscussionStarter } from "./discord-hook/auto-discussion.js";
import { startDiscordGateway } from "./discord-hook/gateway.js";
import { startSocketMode } from "./slack/socket-mode.js";
import { createSlackRouter } from "./slack/slack-router.js";
import { GameFeedbackStore } from "./feedback/store.js";
import { ensureReactionTables, recordPostedMessage, applyReaction } from "./discord-hook/reactions.js";
import { queueRoutes } from "./api/queue-routes.js";
import { kgMigrationRoutes } from "./api/kg-migration-routes.js";
import { startKgSyncScheduler } from "./kg-sync/scheduler.js";
import { createNoiseRoutes } from "./api/noise-routes.js";
import { datasourceRoutes } from "./api/datasource-routes.js";
import { buildQueueSnapshot, formatQueueText } from "./queue/snapshot.js";
import { startBackupScheduler } from "./backup/runner.js";
import { startCostRelay } from "./flow/cost-relay.js";
import { getFlowDb } from "./flow/db/connection.js";
import { createLlmSummarizer } from "./crawler/sources/summarize.js";
import { getConfig } from "./config.js";
import { createEconomyGraphRoutes } from "./api/economy-graph-routes.js";
import { analyzeEconomy, toSlug } from "./ludus/economy-analyzer.js";

// Initialize DB (triggers schema creation)
import "./db/connection.js";

// FEATURE ②: console.error/warn を error-buffer に取り込む (admin dashboard 可視化用)。
// なるべく早く仕掛けて以降の全 console 出力を拾う。
installConsoleCapture();

const config = getConfig();
const app = new Hono();

const frontendUrl = config.server.frontendUrl;

app.use("*", cors({
  origin: frontendUrl,
  credentials: true,
  allowMethods: ["GET", "POST", "PUT", "DELETE", "OPTIONS"],
  allowHeaders: ["Content-Type", "Authorization", "X-User-Id", "X-User-Role"],
}));

// Health check (認証不要E
app.get("/health", (c) => c.json({ status: "ok", service: "discutere" }));

// ─── トップページ (各 Web UI への入口、 認証不要・loopback) ──────────
app.route("/", topPageRoutes);

// 使い方ガイド (Discord フォーラム / チャンネルの使い方とルール)。実 config を注入。
app.route("/", createGuideRoutes(config.discord));

app.route("/", learningViewRoutes);

// 常駐ワーカー callback (内部用、認証不要)。userContext より前に mount する。
app.route("/internal", workerRoutes);

// ─── 認証ミドルウェア (X-User-Id / X-User-Role ヘッダーめEcontext に載せめE ──
// Cernere / 独自 JWT 認証層は Discord-only pivot で撤去。実認可は Discord
// Gateway (bot token + admin-id allowlist) 側。詳細は middleware/auth.ts、E
app.use("/api/*", userContext());

// ─── MACHINA routes ──────────────────────────────────────────
app.route("/api", machinaRoutes);

// ─── Admin (PR-B: 人閁EↁEAI 介�E経路) ────────────────────────
app.route("/api", adminRoutes);

// ─── Admin dashboard HTML (PR-H: 観寁E+ kill switch GUI) ────
app.route("/api", dashboardRoutes);

// ─── エラー可視化 (FEATURE ②: console.error/warn を dashboard に出す) ──
app.route("/api", errorsRoutes);

// ─── 合意スコア可視化 (FEATURE ③+④: 意見ごとの 👍/agree/score) ──
app.route("/api", consensusRoutes);

// ─── 議論キュー可視化 (進行中 session / 未処琁Egap / 検証征E��仮説) ──
app.route("/api", queueRoutes);

// ─── ノイズ管理 (議論データから別ゲーム/煽り等のノイズ発話を除外) ──
app.route("/api", createNoiseRoutes());

// ─── データ調整 UX (データソース閲覧 / 除外 / active KG 指定、 §13) ──
app.route("/api", datasourceRoutes);

// ─── 常駐ワーカー制御 UI/API (/api/worker-pool) ────────────────
app.route("/api", workerPoolControlRoutes);

// ─── 議論チューニング UI/API (/api/admin/tuning) ───────────────
app.route("/api", tuningRoutes);

// ─── KG 共有同期: 配信エンドポイント (master) /api/kg/migrations ──────
app.route("/api", kgMigrationRoutes);
// follower 側の自動 pull (config.kgSync.auto + sourceUrl のときのみ)。
const stopKgSync = startKgSyncScheduler(config);

// PR-C: mode-state TTL cleanup めE15 min interval で起勁E(24h 経過 session を回叁E
const stopSessionCleanup = startSessionCleanup();

let autoDiscussionLlm: LLMClient | null = null;
// 投稿→議題化の分類器専用 LLM (persona engine の worker-pool 経路とは独立)。
// config.classifier に従い Haiku を Lictor 経由 (claude-cli) / API (anthropic) で呼ぶ。
let classifierLlm: LLMClient | null = null;
// backend=worker-pool 時に握る常駐ワーカープール (shutdown で stop する)。
let workerPool: WorkerPool | null = null;
// worker-pool 時の 8 ペルソナ seed (persona id = worker id)。
let workerPersonaSeeds: PersonaSeed[] | undefined;
const isWorkerPool = config.llm.backend === "worker-pool";

/**
 * 分類器 (classifyDiscordMessage) 専用 LLM client を config.classifier に従って構築。
 * persona engine の worker-pool ルーティングとは独立。Haiku を既定とし、
 *   - backend=claude-cli → ClaudeCliClient (Lictor 経由 spawn、サブスク・トークン不要)
 *   - backend=anthropic  → AnthropicSdkClient (API 直)
 *   - backend=off / 資格情報無し → null (classifyDiscordMessage が regex fallback)
 */
function buildClassifierLlm(): LLMClient | null {
  const c = config.classifier;
  if (c.backend === "off") {
    console.log("  classifier LLM: off (regex fallback のみ)");
    return null;
  }
  if (c.backend === "anthropic") {
    const apiKey = config.llm.anthropicApiKey;
    if (!apiKey) {
      console.log("  classifier LLM: anthropic 指定だが ANTHROPIC_API_KEY 無し → regex fallback");
      return null;
    }
    console.log(`  classifier LLM: AnthropicSdkClient (model=${c.model})`);
    return new AnthropicSdkClient({ apiKey, defaultModel: c.model });
  }
  if (c.backend === "local") {
    // 分類は llm.local エンドポイントを共用 (model だけ classifier.model で上書き)。
    console.log(`  classifier LLM: LocalOpenAiClient (${config.llm.local.baseUrl} / model=${c.model})`);
    return new LocalOpenAiClient({
      baseUrl: config.llm.local.baseUrl,
      defaultModel: c.model || config.llm.local.model,
      apiKey: config.llm.local.apiKey,
      defaultTimeoutMs: config.classifier.timeoutMs,
    });
  }
  // claude-cli = Lictor 経由 spawn。
  console.log(`  classifier LLM: ClaudeCliClient / Lictor 経由 spawn (model=${c.model})`);
  return new ClaudeCliClient({
    defaultModel: c.model,
    defaultTimeoutMs: c.timeoutMs,
    gitBashPath: config.workerPool.gitBashPath ?? config.llm.gitBashPath,
  });
}

// フォーラム集約: 収束したフォーラムポストを締める finalizer。
// facilitator は gateway より先に生成されるため late-bound (gateway 起動後に結線)。
let forumFinalizer:
  | ((args: { scene: string | null; summary: string; title: string }) => void)
  | null = null;

// FEATURE ③+④: 合意スコアラーが 👍 を付けるための react。 gateway 起動後に
// handle.reactToMessage を late-bind する (forumFinalizer と同じパターン)。
let consensusReact:
  | ((channelId: string, messageId: string, emoji: string) => Promise<void>)
  | null = null;

// PR-C / PR-I: persona-engine 起勁Ewiring
//   LLM backend は config.llm.backend で刁E��: "anthropic" (= anthropicApiKey)
//   また�E "claude-cli" (= Lictor 経由 spawn、E環墁E�� claude CLI が忁E��E、E
//   または "worker-pool" (= サブスク Lictor 常駐ワーカー、 spec/feature/persistent-worker-pool.md)
const personaEngineLifecycle = (() => {
  let llm: LLMClient | null = null;

  // runtime 設定ストア (収束トリガー / 役割プロンプト / debate instructions の
  // override を SQLite 永続)。 役割プロンプト override を persona seed / standing
  // prompt 生成より前に効かせるため、 ここで最初に作って resolver を注入する。
  const settingsDb = new Database("./data/discutere-settings.db");
  const runtimeSettings = createRuntimeSettingsStore(settingsDb, {
    facilitator: {
      tickMs: config.facilitator.tickMs,
      idleGapMs: config.facilitator.idleGapMs,
      maxPersonas: config.facilitator.maxPersonas,
      aufhebungTarget: config.facilitator.aufhebungTarget,
      convergePolicy: "default",
    },
    rolePrompts: ROLE_GUIDANCE_DEFAULTS,
    ruleInstructions: RULE_INSTRUCTION_DEFAULTS,
  });
  setRolePromptResolver((role) => runtimeSettings.getRolePrompt(role));
  setRuntimeSettings(runtimeSettings);

  if (isWorkerPool) {
    const allWorkers =
      config.workerPool.workers.length > 0 ? config.workerPool.workers : DEFAULT_WORKERS;
    // 議論パーティ設定: excludeProviders (例 ["codex"]) の provider を外す。
    // トークンの無い provider を除外して claude のみで回す等の用途。
    const excluded = new Set(config.workerPool.excludeProviders);
    const workers =
      excluded.size > 0 ? allWorkers.filter((w) => !excluded.has(w.provider)) : allWorkers;
    if (excluded.size > 0) {
      console.log(
        `  worker-pool: excludeProviders=[${[...excluded].join(",")}] → 参加 ${workers.length}/${allWorkers.length} 名`
      );
    }
    workerPool = new WorkerPool(
      {
        enabled: config.workerPool.enabled,
        workspace: config.workerPool.workspace,
        callbackBaseUrl: config.workerPool.callbackBaseUrl,
        // ワーカー (lictor claude/codex) の cwd = 専用 worker-home。
        // そこの .claude/settings.json (edit-mode + register/send allow-list) を効かせる。
        workerCwd: join(process.cwd(), "worker-home"),
        gitBashPath: config.workerPool.gitBashPath,
        injectDelayMs: config.workerPool.injectDelayMs,
        turnTimeoutMs: config.workerPool.turnTimeoutMs,
        registerTimeoutMs: config.workerPool.registerTimeoutMs,
        registerSettleMs: config.workerPool.registerSettleMs,
        turnsDir: "data/worker-turns",
        promptsDir: "data/worker-prompts",
        workers,
      },
      process.cwd()
    );
    setWorkerPool(workerPool);
    setWorkerPoolControl(workerPool);
    workerPersonaSeeds = buildWorkerPersonaSeeds(workers);
    // boot 自動 spawn は config.workerPool.enabled が true の時だけ (既定 false)。
    // 通常は /api/worker-pool の UI から必要数だけ手動起動する。
    if (config.workerPool.enabled) workerPool.start();
    // worker 未起動のペルソナは既存どおり claude -p で動作させる。
    // model は worker 定義 (delegation) に沿わせる。
    const workerFallback = new ClaudeCliClient({
      defaultTimeoutMs: config.llm.claudeCliTimeoutMs,
      gitBashPath: config.workerPool.gitBashPath ?? config.llm.gitBashPath,
    });
    llm = new WorkerPoolClient(workerPool, workerFallback);
    console.log(
      `  persona-engine LLM: WorkerPoolClient (定義 ${workers.length} / boot自動起動=${config.workerPool.enabled}, 未起動は claude -p フォールバック)`
    );
  } else if (config.llm.backend === "local") {
    llm = new LocalOpenAiClient({
      baseUrl: config.llm.local.baseUrl,
      defaultModel: config.llm.local.model,
      apiKey: config.llm.local.apiKey,
      defaultTimeoutMs: config.llm.local.timeoutMs,
    });
    console.log(
      `  persona-engine LLM: LocalOpenAiClient (${config.llm.local.baseUrl} / model=${config.llm.local.model})`
    );
  } else if (config.llm.backend === "claude-cli") {
    llm = new ClaudeCliClient({
      defaultTimeoutMs: config.llm.claudeCliTimeoutMs,
      defaultModel: config.llm.model,
      gitBashPath: config.llm.gitBashPath,
    });
    console.log("  persona-engine LLM: ClaudeCliClient (Lictor 経由 spawn)");
  } else if (config.llm.anthropicApiKey) {
    llm = new AnthropicSdkClient({
      apiKey: config.llm.anthropicApiKey,
      defaultModel: config.llm.model,
    });
    console.log("  persona-engine LLM: AnthropicSdkClient (HTTP)");
  } else {
    console.log("  persona-engine: skipped (set llm.backend=claude-cli or llm.anthropicApiKey)");
    return null;
  }
  autoDiscussionLlm = llm;

  // 分類器 (classifyDiscordMessage) 専用 LLM を persona engine とは別建てで構築する。
  // persona の WorkerPoolClient は personaId ルーティング前提なので、personaId を持たない
  // 分類呼び出しを通すと ok:false → regex fallback に落ちてしまう (LLM が効かない)。
  // ここで Haiku を Lictor 経由 (claude-cli) / API (anthropic) で直接呼べる client を持つ。
  classifierLlm = buildClassifierLlm();

  try {
    // worker-pool は persona DB だけ分離し (8 固定キャスト)、workspace は
    // 実フォーラムと共有する (= worker が live forum の utterance/gap を読む)。
    // workspace まで分けると worker が空の議論を見て反応しなくなる。
    const peDbPath = isWorkerPool ? "./data/persona-engine-debate.db" : config.personaEngine.dbPath;
    const workspaceId = config.workspace;
    const peDb = new Database(peDbPath);
    const core = createCore(resolveActiveKgPath(config));
    ensureReactionTables(core.client.raw);
    const relayPersonas = new PersonasRepo(peDb);
    // persona の人間名を解決する。 全 persona (進行役 facilitator 含む) を
    // webhook で人間名として投稿する (役割名は出さない)。
    const resolveSpeaker = (byPersonaId: string) => {
      const p = relayPersonas.get(byPersonaId);
      return {
        name: p?.display_name ?? byPersonaId,
        viaWebhook: true,
      };
    };
    const adapter = createDiscatierContextProvider(core, {
      // PR-I: AI 発話 / hypothesis を Discord channel に post
      async onPostedUtterance(input) {
        const sp = resolveSpeaker(input.byPersonaId);
        // ログにも人間名を反映 (誰が何を言ったか追える)。
        console.log(`  [議論] ${sp.name}: ${input.text.replace(/\s+/g, " ").slice(0, 80)}`);
        const r = await postDiscussionToDiscord({
          core,
          workspaceId: input.workspaceId,
          sessionId: input.sessionId,
          kind: "utterance",
          speakerLabel: sp.name,
          viaWebhook: sp.viaWebhook,
          text: input.text,
        });
        if (!r.ok && !r.skipped) {
          console.warn(`  persona-engine: discord utterance post skipped (${sp.name}):`, r.reason);
        } else if (r.messageId) {
          recordPostedMessage(core.client.raw, {
            messageId: r.messageId,
            targetId: input.utteranceId,
            targetKind: "utterance",
            channelId: r.channelId,
          });
        }
      },
      async onPostedHypothesis(input) {
        // hypothesis は対応すめEgap session の scene ぁEdiscord 由来でなぁE��合スキチE�E
        const session = core.client.raw
          .prepare(
            "SELECT id, scene FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
          )
          .get(workspaceId, `discussion-of-gap:${input.designGapId ?? "_none"}`) as
          | { id: string; scene: string | null }
          | undefined;
        if (!session?.scene?.startsWith("discord:")) return;
        const sp = resolveSpeaker(input.byPersonaId);
        const r = await postDiscussionToDiscord({
          core,
          workspaceId: input.workspaceId,
          sessionId: session.id,
          kind: "hypothesis",
          speakerLabel: sp.name,
          viaWebhook: sp.viaWebhook,
          text: input.statement,
        });
        if (!r.ok) {
          console.warn("  persona-engine: discord hypothesis post skipped:", r.reason);
        } else if (r.messageId) {
          recordPostedMessage(core.client.raw, {
            messageId: r.messageId,
            targetId: input.hypothesisId,
            targetKind: "hypothesis",
            channelId: r.channelId,
          });
        }
      },
    });
    // 参加 persona の id 集合 (excludeProviders 適用後)。 除外された persona を
    // target にする rule は seed しない (= GPT 除外時に GPT ルールを出さない)。
    const activeWorkerIds = new Set((workerPersonaSeeds ?? []).map((p) => p.id));
    const engine = createPersonaEngine({
      db: peDb,
      llm,
      contextProvider: adapter,
      workspaceId,
      // worker-pool 時は固定キャストを seed (persona id = worker id)。
      personaSeeds: workerPersonaSeeds,
      // worker-pool 時は target=worker id の debate ルールを使う
      // (既存ルールの target=advocate 等は worker と一致せず全 skip になる)。
      // instructions は runtime override (チューニング UI) を適用してから seed する。
      ruleSeeds: isWorkerPool
        ? applyRuleInstructionOverrides(
            DEBATE_RULE_SEEDS.filter((r) => !r.target || activeWorkerIds.has(r.target)),
            (id) => runtimeSettings.getRuleInstruction(id),
          )
        : undefined,
      maxFiresPerSession: config.personaEngine.maxFiresPerSession,
      maxFiresPerRulePerSession: config.personaEngine.maxFiresPerRule,
      tickMs: config.personaEngine.tickMs,
    });
    const bridge = createEventBridge(core, engine, {
      workspaceId,
      pollMs: config.personaEngine.bridgePollMs,
    });
    // excludeProviders で外した persona の rule が既存 DB に残っていると
    // 「persona not found」 を吐き続けるので、 active でない target の rule を disable。
    if (isWorkerPool && config.workerPool.excludeProviders.length > 0) {
      let disabledCount = 0;
      for (const r of engine.rules.list({ enabled: true })) {
        if (r.target && !activeWorkerIds.has(r.target)) {
          engine.rules.setEnabled(r.id, false);
          disabledCount++;
        }
      }
      if (disabledCount > 0) {
        console.log(`  worker-pool: 除外 provider の rule ${disabledCount} 件を無効化`);
      }
    }
    setPersonaEngine(engine);
    // 旧 persona-engine は既定で起動しない (議論は新フロー src/flow に集約済み, OVERVIEW §4)。
    // LLM クライアント (worker-pool) / peDb / core の構築は上で済ませており enabled に依らず維持する。
    if (config.personaEngine.enabled) {
      engine.start();
      bridge.start();
      console.log(
        `  persona-engine: attached (workspace=${workspaceId}, db=${peDbPath})`
      );
    } else {
      console.log(
        "  persona-engine: disabled (新フロー集約のため起動しない / DISCUTERE_PERSONA_ENGINE_ENABLED=1 で有効化)"
      );
    }

    // ファシリテーター: 停滞→新 persona 投入で拡張、 persona 過多→収束 (gap closed)
    // worker-pool 時は動的 persona 生成 (= ワーカー無しの persona) を避けるため facilitator を止める。
    // facilitator (司会) の converge/expand は persona ルーティングを伴わない単発生成なので、
    // worker-pool 時は WorkerPoolClient ではなく直 claude -p を使う。WorkerPoolClient は
    // personaId を要求し、 facilitator の invoke は personaId を持たないため「ルーティング不能」で
    // 失敗する (= まとめ生成不可)。非 worker-pool 時は llm (claude-cli/anthropic) をそのまま使う。
    const facilitatorLlm: LLMClient | null = isWorkerPool
      ? new ClaudeCliClient({
          defaultTimeoutMs: config.llm.claudeCliTimeoutMs,
          gitBashPath: config.workerPool.gitBashPath ?? config.llm.gitBashPath,
          defaultModel: config.llm.model || undefined,
        })
      : llm;
    let facilitator: ReturnType<typeof createFacilitator> | null = null;
    if (config.personaEngine.enabled && config.facilitator.enabled && facilitatorLlm) {
      const facLogger = {
        debug: () => {},
        info: (meta: Record<string, unknown>, msg: string) => console.log(`  [facilitator] ${msg}`, meta),
        warn: (meta: Record<string, unknown>, msg: string) => console.warn(`  [facilitator] ${msg}`, meta),
        error: (meta: Record<string, unknown>, msg: string) => console.error(`  [facilitator] ${msg}`, meta),
      };
      facilitator = createFacilitator({
        core,
        llm: facilitatorLlm,
        contextProvider: adapter,
        personas: new PersonasRepo(peDb),
        workspaceId,
        logger: facLogger,
        options: {
          tickMs: config.facilitator.tickMs,
          idleGapMs: config.facilitator.idleGapMs,
          maxPersonas: config.facilitator.maxPersonas,
          aufhebungTarget: config.facilitator.aufhebungTarget,
          model: config.llm.model,
          tierModels: config.llm.tiers,
        },
        // 収束トリガーは tick ごとに runtime-settings から読み、 チューニング UI の
        // 変更 (「20」等 / aufhebung-only ポリシー) を再起動なしで反映する。
        tuning: () => runtimeSettings.getFacilitatorTuning(),
        // 収束したらフォーラムポストを締める (gateway 起動後に forumFinalizer が結線される)。
        onConverged: (e) =>
          forumFinalizer?.({ scene: e.scene, summary: e.summary, title: e.title }),
      });
      // facilitator は常に auto-tick を回す: 停滞→拡張、 止揚到達/persona 過多→自動収束
      // (= gap closed + onConverged で Discord フォーラムを lock+archive + まとめ転記)。
      // worker-pool 時も converge/expand は専用 claude -p で動く (facilitatorLlm)。
      facilitator.start();
      console.log(
        `  facilitator: started (tick=${config.facilitator.tickMs}ms, idleGap=${config.facilitator.idleGapMs}ms, maxPersonas=${config.facilitator.maxPersonas}, workerPool=${isWorkerPool})`
      );
      setFacilitator(facilitator);
    }

    // FEATURE ③+④: 合意スコアラー — 各意見 (AI/人間 同一) を AI が評価し、 総意が
    // 同意する意見に 👍 + 合意スコアを記録する。 facilitator と同じ LLM / tick で回す。
    // react は gateway 起動後に late-bind される consensusReact に委譲する。
    let consensusScorer: ConsensusScorer | null = null;
    if (config.personaEngine.enabled && config.facilitator.enabled && facilitatorLlm) {
      consensusScorer = createConsensusScorer({
        core,
        llm: facilitatorLlm,
        intervalMs: config.facilitator.tickMs,
        model: isWorkerPool ? config.llm.model || undefined : config.llm.model,
        react: async (i) => {
          if (consensusReact) await consensusReact(i.channelId, i.messageId, i.emoji);
        },
        logger: {
          debug: () => {},
          info: (meta: Record<string, unknown>, msg: string) => console.log(`  [consensus] ${msg}`, meta),
          warn: (meta: Record<string, unknown>, msg: string) => console.warn(`  [consensus] ${msg}`, meta),
          error: (meta: Record<string, unknown>, msg: string) => console.error(`  [consensus] ${msg}`, meta),
        },
      });
      consensusScorer.start();
      console.log(`  consensus-scorer: started (interval=${config.facilitator.tickMs}ms)`);
    }

    // 自動シード議論: 定期的にジャンル/ストアトレンドから headless 議論を立てる (#64/#65)。
    // 駆動は上の facilitator が担うので、 facilitator が動いている時だけ有効化する。
    // (worker-pool 時は facilitator を止めているので auto-seed も起動しない)
    let autoSeed: ReturnType<typeof createAutoSeedScheduler> | null = null;
    if (config.autoSeed.enabled && facilitator && !isWorkerPool) {
      autoSeed = createAutoSeedScheduler({
        core,
        personas: new PersonasRepo(peDb),
        workspaceId,
        config: {
          intervalMs: config.autoSeed.intervalMs,
          maxConcurrent: config.autoSeed.maxConcurrent,
          sources: config.autoSeed.sources,
        },
        logger: {
          debug: () => {},
          info: (meta: Record<string, unknown>, msg: string) => console.log(`  [auto-seed] ${msg}`, meta),
          warn: (meta: Record<string, unknown>, msg: string) => console.warn(`  [auto-seed] ${msg}`, meta),
          error: (meta: Record<string, unknown>, msg: string) => console.error(`  [auto-seed] ${msg}`, meta),
        },
      });
      autoSeed.start();
      console.log(
        `  auto-seed: started (interval=${config.autoSeed.intervalMs}ms, maxConcurrent=${config.autoSeed.maxConcurrent}, sources=${config.autoSeed.sources.join("/")})`
      );
    }

    return { engine, bridge, core, peDb, facilitator, autoSeed, consensusScorer };
  } catch (err) {
    console.warn("  persona-engine: startup failed:", err);
    return null;
  }
})();

// ─── ゲーム経済グラフ (GET /ludus/economy-graph/:slug, POST /api/ludus/analyze-economy) ───
{
  const economyApiKey = config.llm.anthropicApiKey ?? process.env.ANTHROPIC_API_KEY ?? "";
  app.route("/", createEconomyGraphRoutes({ workspaceId: config.workspace, apiKey: economyApiKey }));
  console.log("  economy-graph: /ludus/economy-graph/:slug — ゲームメカニクス経済グラフ");
}

// ─── 軽量 Web チャット UI (/chat) — Discord 非依存の議論経路 ───
// scene=web:<room> で同じ議論エンジン (分類器 → designGap → persona/facilitator) を再利用する。
// 分類器は gateway と同じ starter を共有し、 persona 表示名は peDb から解決する。
{
  const webPersonas = personaEngineLifecycle ? new PersonasRepo(personaEngineLifecycle.peDb) : null;
  setWebChatDeps({
    workspaceId: config.workspace,
    classifyInboundMessage: createDiscordAutoDiscussionStarter({ getLlm: () => classifierLlm }),
    resolveSpeakerName: (personaId) => webPersonas?.get(personaId)?.display_name ?? personaId,
  });
  app.route("/", webChatRoutes);
  console.log("  web-chat: /chat (loopback) — scene=web:<room> で議論可能");
}

// ─── 議論フローの LLM 解決 (worker-pool 踏襲 + 無ワーカー時 claude -p フォールバック) ───
// 新フロー (FlowDirector) は invoke に personaId を渡さない呼び出し (facilitator/投票/
// 結論/要約/感情) が多い。worker-pool backend の WorkerPoolClient (autoDiscussionLlm) は
// personaId 未指定だと即 ok:false でフォールバックしないため、そのまま繋ぐとフローが壊れる。
// FallbackLlm で worker-pool を試行 → 外れたら claude -p に回す。worker が起動していれば
// 発話は worker に流れ、いなければ claude -p。backend が worker-pool 以外なら persona-engine
// の llm (claude-cli/local/anthropic) をそのまま使い、それも無ければ classifier にフォールバック。
const flowEngineLlm: LLMClient | null = (() => {
  if (autoDiscussionLlm && config.llm.backend === "worker-pool") {
    // E: SDK (サブスク OAuth + cache_control)。Claude Code 認証トークンを動的に読む。
    // usage が返るので cost-logger のトークン計上が有効化する。
    const sdk = new AnthropicSdkClient({
      getAuthToken: () => readClaudeCodeToken(),
      apiKey: config.llm.anthropicApiKey || undefined,
      defaultModel: config.llm.model || undefined,
      enableCache: true,
    });
    const cliFallback = new ClaudeCliClient({
      defaultTimeoutMs: config.llm.claudeCliTimeoutMs,
      defaultModel: config.llm.model || undefined,
      gitBashPath: config.workerPool.gitBashPath ?? config.llm.gitBashPath,
    });
    // 鎖: worker-pool (personaId ルート) → SDK(OAuth/cache) → claude-p。
    // フローは personaId 無し呼び出しが主なので実質 SDK が効き、トークン無し時のみ claude-p。
    return new FallbackLlm(autoDiscussionLlm, new FallbackLlm(sdk, cliFallback));
  }
  return autoDiscussionLlm ?? classifierLlm;
})();

// ─── 議論フロー WebUI (/flow) — 4 フロー (議論/改善/学習/壁打ち) の正式入口 (T7) ───
// テーマ + 議論タイプ (必須) + タグ を受けて dispatch する。LLM backend がある時のみ有効。
if (flowEngineLlm) {
  const flowLlm = flowEngineLlm;
  setFlowWebDeps({
    workspaceId: config.workspace,
    llm: flowLlm,
    openCore: () => createCore(resolveActiveKgPath(config)),
    sentimentClients: { main: flowLlm },
  });
  app.route("/", flowRoutes);
  console.log("  flow-ui: /flow (loopback) — 議論/改善/学習/壁打ち を起動");
}

const port = config.server.port;

// ─── S3 バックアチE�E: 月次自動スケジューラ起勁E(enabled かつ bucket 設定時のみ) ──
//   手動トリガ (slash /discutere-backup・npm run backup) は scheduler.trigger() を�E有、E
const backupScheduler = startBackupScheduler(config);

// ─── LLM コストの Anatomia への定期 relay (cost.relay.enabled + URL 設定時のみ) ──
//   llm_call_log のセッション別累積を Anatomia コスト削減UI (POST /api/cost-feed) へ PUSH。
//   手動は npm run cost-relay。送信失敗は議論を止めない (graceful)。
if (config.cost.relay.enabled && config.cost.relay.anatomiaUrl) {
  startCostRelay(getFlowDb(), {
    baseUrl: config.cost.relay.anatomiaUrl,
    service: config.cost.relay.service,
    intervalMs: config.cost.relay.intervalMs,
  });
  console.log(
    `  cost-relay: started (-> ${config.cost.relay.anatomiaUrl}, interval=${config.cost.relay.intervalMs}ms)`
  );
}

// 議論キューのサマリ生�E (slash /discutere-queue 用)。core は都度 open/close、E
function buildQueueText(): string {
  const core = createCore(resolveActiveKgPath(config));
  try {
    const peDb = personaEngineLifecycle?.peDb ?? null;
    return formatQueueText(buildQueueSnapshot(core.client.raw, peDb, config.workspace));
  } finally {
    core.close();
  }
}

// WS 再設訁E Discord Gateway 常時接綁E(公閁EURL + 署名検証エンド�Eイント不要E、E
//   config.discord.botToken 未設定なめEskip 起動、E
// ゲーム感想の匿名収集ストア (専用 sqlite)。
const gameFeedbackStore = new GameFeedbackStore(new Database("./data/game-feedback.db"));

const discordGatewayLifecycle = startDiscordGateway({
  botToken: config.discord.botToken ?? "",
  applicationId: config.discord.applicationId,
  guildIds: config.discord.guildIds,
  workspaceId: config.workspace,
  adminIds: config.discord.adminIds,
  discussionChannelIds: config.discord.discussionChannelIds,
  // データクロール用チャンネル: 貼られた URL から外部議論データを取り込む。
  crawlChannelIds: config.discord.crawlChannelIds,
  // フォーラム集約: guild 内の全 Forum 監視 + データ学習依頼/まとめ投稿 を自動作成。
  forum: config.discord.forum,
  // 新フロー (議論/改善/学習/壁打ち) の Discord live 実行依存。LLM backend がある時のみ。
  // 設定するとフォーラムスレッドは新フローエンジンで起動する (旧 auto-discussion 経路は不使用)。
  flowLive: flowEngineLlm
    ? {
        botToken: config.discord.botToken ?? "",
        llm: flowEngineLlm,
        openCore: () => createCore(resolveActiveKgPath(config)),
        sentimentClients: { main: flowEngineLlm },
        workspaceId: config.workspace,
      }
    : undefined,
  // /debate のパーティ議論 (司会+キーマン+意見、想定発話数で続行/停止)。
  debate: config.discussion,
  gitBashPath: config.workerPool.gitBashPath ?? config.llm.gitBashPath,
  // ゲーム感想チャンネル: カテゴリ「ゲーム感想」配下の投稿を匿名で意見データ収集。
  gameFeedback: config.discord.gameFeedback,
  onGameFeedback: (fb) => {
    const r = gameFeedbackStore.add(fb);
    if (r) console.log(`  game-feedback: 収集 [${fb.gameTitle}] ${fb.content.replace(/\s+/g, " ").slice(0, 60)}`);
  },
  crawlDeps: {
    createCore: () => createCore(resolveActiveKgPath(config)),
    workspaceId: config.workspace,
    youtubeApiKey: process.env.DISCUTERE_YOUTUBE_API_KEY ?? null,
    reddit:
      process.env.DISCUTERE_REDDIT_CLIENT_ID && process.env.DISCUTERE_REDDIT_CLIENT_SECRET
        ? {
            clientId: process.env.DISCUTERE_REDDIT_CLIENT_ID,
            clientSecret: process.env.DISCUTERE_REDDIT_CLIENT_SECRET,
            userAgent:
              process.env.DISCUTERE_REDDIT_USER_AGENT ?? "LUDIARS-Discutere/0.1 (external discussion crawler)",
          }
        : null,
    // website 長文の要約器 (id=67)。 LLM があれば要約/raw 2 層で取り込む。
    summarizer: autoDiscussionLlm ? createLlmSummarizer(autoDiscussionLlm, { model: config.llm.model }) : null,
  },
  getEngine: () => getPersonaEngine(),
  buildQueueText,
  triggerBackup: () => backupScheduler.trigger(),
  triggerEconomyAnalysis: (gameTitle: string) => {
    const apiKey = config.llm.anthropicApiKey ?? process.env.ANTHROPIC_API_KEY ?? "";
    void analyzeEconomy(config.workspace, gameTitle, apiKey).catch(
      (err) => console.error(`  economy-graph: analysis failed: ${(err as Error).message}`),
    );
    return { slug: toSlug(gameTitle), port };
  },
  // 憑依対象ペルソナの生成 (item7): クロール済み外部発話から C1 採用して flow_persona へ upsert。
  triggerPersonaGenerate: async (source?: string) => {
    const summary = runAdoptFromKg({ sourceFilter: source, dry: false });
    return { adopted: summary.adopted, speakers: summary.speakers };
  },
  classifyInboundMessage: createDiscordAutoDiscussionStarter({
    // 分類器は専用 LLM (Haiku / Lictor or API)。persona engine の summarizer
    // (autoDiscussionLlm) とは別系統 — facilitator のモデルには影響しない。
    getLlm: () => classifierLlm,
  }),
  // 議論意見へのリアクション → 内部スコア加算 (絵文字ごとの重み)。
  onReaction: (info) => {
    const pe = personaEngineLifecycle;
    if (!pe?.core) return;
    try {
      applyReaction(pe.core.client.raw, { messageId: info.messageId, emoji: info.emoji });
    } catch (err) {
      console.warn("  discord-gateway: reaction scoring failed:", (err as Error).message);
    }
  },
}).catch((err) => {
  console.warn("  discord-gateway: startup failed:", (err as Error).message);
  return null;
});

// フォーラム集約: gateway 起動後に収束 finalizer を結線する (facilitator.onConverged が呼ぶ)。
discordGatewayLifecycle
  .then((handle) => {
    if (!handle) return;

    // FEATURE ③+④: 合意スコアラーの 👍 を gateway の reactToMessage に結線。
    consensusReact = (channelId, messageId, emoji) =>
      handle.reactToMessage(channelId, messageId, emoji);

    // FEATURE ⑤b: 未検知投稿の再スイープを admin API (/api/admin/seed-sweep) に結線。
    setGatewaySweeper((opts) => handle.sweepUnseeded(opts));
    console.log("  discord-seed-sweep / consensus-react: wired to gateway");

    if (!config.discord.forum.enabled) return;
    forumFinalizer = (args) => {
      void handle
        .finalizeForumPost(args)
        .then((r) => {
          if (r.closed) console.log("  discord-forum: post closed (converged)");
        })
        .catch((e) => console.warn("  discord-forum: finalize error:", (e as Error).message));
    };
    console.log("  discord-forum: convergence finalizer wired");
  })
  .catch(() => {});

// ─── Slack Socket Mode 起動 (Discord と並ぶ第2トランスポート) ───
//   appToken/botToken/channelIds が揃い enabled かつ LLM backend がある時のみ起動。
let slackSocket: { stop(): void } | null = null;
if (
  config.slack.enabled &&
  config.slack.appToken &&
  config.slack.botToken &&
  config.slack.channelIds.length > 0 &&
  flowEngineLlm
) {
  const slackRouter = createSlackRouter({
    channelIds: config.slack.channelIds,
    deps: {
      botToken: config.slack.botToken,
      llm: flowEngineLlm,
      openCore: () => createCore(resolveActiveKgPath(config)),
      sentimentClients: { main: flowEngineLlm },
      workspaceId: config.workspace,
    },
  });
  slackSocket = startSocketMode({
    appToken: config.slack.appToken,
    onEvent: slackRouter.onEvent,
    onInteractive: slackRouter.onInteractive,
  });
  console.log("  slack: Socket Mode 起動 (議論/改善/学習/壁打ち)");
}

console.log(`Discutere listening on http://localhost:${port}`);
console.log(`  Auth:     Discord Gateway (bot token + admin-id allowlist) / HTTP は X-User-Id・X-User-Role ヘッダー`);
console.log(`  Tasks:    /api/groups/:id/tasks`);
console.log(`  Monitors: /api/groups/:id/monitors`);
console.log(`  Webhooks: /api/webhook/slack, /api/webhook/discord`);
console.log(`  Discord:  Gateway (WS)  Eslash + message via bot token`);
console.log(`  Admin:    /api/admin/{rules/enabled,session/reset,status}`);
console.log(`  Dashboard: /api/admin/dashboard (HTML, admin role)`);
console.log(`  Learning:  /learning (local read-only view)`);
console.log(`  Analyze:  /api/analyze`);

if (isWorkerPool) {
  console.log(`  WorkerPool: /internal/worker/{register,utterance} (常駐ワーカー callback)`);
}

// 終了時に常駐ワーカー (8 セッション) を kill する。
let shuttingDown = false;
const gracefulShutdown = (sig: string) => {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`\n${sig} received — shutting down (worker pool stop)...`);
  try {
    workerPool?.stop();
  } catch (err) {
    console.warn("  worker-pool stop failed:", (err as Error).message);
  }
  try {
    stopSessionCleanup();
  } catch {
    /* best-effort */
  }
  try {
    stopKgSync();
  } catch {
    /* best-effort */
  }
  try {
    slackSocket?.stop();
  } catch {
    /* best-effort */
  }
  process.exit(0);
};
process.on("SIGINT", () => gracefulShutdown("SIGINT"));
process.on("SIGTERM", () => gracefulShutdown("SIGTERM"));

serve({ fetch: app.fetch, port });
