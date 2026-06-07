import { join } from "node:path";

import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { cors } from "hono/cors";
import Database from "better-sqlite3";
import { machinaRoutes } from "./machina/routes.js";
import { userContext } from "./middleware/auth.js";
import { adminRoutes, setPersonaEngine, getPersonaEngine } from "./api/admin-routes.js";
import { dashboardRoutes } from "./api/dashboard-routes.js";
import { learningViewRoutes } from "./api/learning-view-routes.js";
import { startSessionCleanup } from "./machina/mode-state.js";
import { createCore } from "./core/index.js";
import { createDiscatierContextProvider } from "./discatier-engine-adapter/index.js";
import { createEventBridge } from "./discatier-engine-adapter/event-bridge.js";
import {
  AnthropicSdkClient,
  ClaudeCliClient,
  createPersonaEngine,
  type LLMClient,
  type PersonaSeed,
} from "./persona-engine/index.js";
import { WorkerPool } from "./persona-engine/worker-pool/pool.js";
import { WorkerPoolClient } from "./persona-engine/worker-pool/client.js";
import { DEFAULT_WORKERS, buildWorkerPersonaSeeds } from "./persona-engine/worker-pool/persona-prompts.js";
import { DEBATE_RULE_SEEDS } from "./persona-engine/worker-pool/debate-rules.js";
import { workerRoutes, setWorkerPool } from "./api/worker.js";
import { workerPoolControlRoutes, setWorkerPoolControl } from "./api/worker-pool-control.js";
import { tuningRoutes, setRuntimeSettings } from "./api/tuning-routes.js";
import { topPageRoutes } from "./api/top-page-routes.js";
import { createRuntimeSettingsStore } from "./runtime-settings/store.js";
import { setRolePromptResolver, ROLE_GUIDANCE_DEFAULTS } from "./persona-engine/worker-pool/persona-prompts.js";
import { applyRuleInstructionOverrides, RULE_INSTRUCTION_DEFAULTS } from "./persona-engine/worker-pool/debate-rules.js";
import { PersonasRepo } from "./persona-engine/db/personas-repo.js";
import { createFacilitator } from "./persona-engine/facilitator/index.js";
import { createAutoSeedScheduler } from "./discussion-seed/scheduler.js";
import { postDiscussionToDiscord } from "./discord-hook/discussion-bridge.js";
import { createDiscordAutoDiscussionStarter } from "./discord-hook/auto-discussion.js";
import { startDiscordGateway } from "./discord-hook/gateway.js";
import { ensureReactionTables, recordPostedMessage, applyReaction } from "./discord-hook/reactions.js";
import { queueRoutes } from "./api/queue-routes.js";
import { buildQueueSnapshot, formatQueueText } from "./queue/snapshot.js";
import { startBackupScheduler } from "./backup/runner.js";
import { createLlmSummarizer } from "./crawler/sources/summarize.js";
import { getConfig } from "./config.js";

// Initialize DB (triggers schema creation)
import "./db/connection.js";

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

// ─── 議論キュー可視化 (進行中 session / 未処琁Egap / 検証征E��仮説) ──
app.route("/api", queueRoutes);

// ─── 常駐ワーカー制御 UI/API (/api/worker-pool) ────────────────
app.route("/api", workerPoolControlRoutes);

// ─── 議論チューニング UI/API (/api/admin/tuning) ───────────────
app.route("/api", tuningRoutes);

// PR-C: mode-state TTL cleanup めE15 min interval で起勁E(24h 経過 session を回叁E
const stopSessionCleanup = startSessionCleanup();

let autoDiscussionLlm: LLMClient | null = null;
// backend=worker-pool 時に握る常駐ワーカープール (shutdown で stop する)。
let workerPool: WorkerPool | null = null;
// worker-pool 時の 8 ペルソナ seed (persona id = worker id)。
let workerPersonaSeeds: PersonaSeed[] | undefined;
const isWorkerPool = config.llm.backend === "worker-pool";

// フォーラム集約: 収束したフォーラムポストを締める finalizer。
// facilitator は gateway より先に生成されるため late-bound (gateway 起動後に結線)。
let forumFinalizer:
  | ((args: { scene: string | null; summary: string; title: string }) => void)
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
    const workers =
      config.workerPool.workers.length > 0 ? config.workerPool.workers : DEFAULT_WORKERS;
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

  try {
    // worker-pool は persona DB だけ分離し (8 固定キャスト)、workspace は
    // 実フォーラムと共有する (= worker が live forum の utterance/gap を読む)。
    // workspace まで分けると worker が空の議論を見て反応しなくなる。
    const peDbPath = isWorkerPool ? "./data/persona-engine-debate.db" : config.personaEngine.dbPath;
    const workspaceId = config.workspace;
    const peDb = new Database(peDbPath);
    const core = createCore();
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
    const engine = createPersonaEngine({
      db: peDb,
      llm,
      contextProvider: adapter,
      workspaceId,
      // worker-pool 時は 8 固定キャストを seed (persona id = worker id)。
      personaSeeds: workerPersonaSeeds,
      // worker-pool 時は target=worker id の debate ルールを使う
      // (既存ルールの target=advocate 等は worker と一致せず全 skip になる)。
      // instructions は runtime override (チューニング UI) を適用してから seed する。
      ruleSeeds: isWorkerPool
        ? applyRuleInstructionOverrides(DEBATE_RULE_SEEDS, (id) =>
            runtimeSettings.getRuleInstruction(id),
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
    setPersonaEngine(engine);
    engine.start();
    bridge.start();
    console.log(
      `  persona-engine: attached (workspace=${workspaceId}, db=${peDbPath})`
    );

    // ファシリテーター: 停滞→新 persona 投入で拡張、 persona 過多→収束 (gap closed)
    // worker-pool 時は動的 persona 生成 (= ワーカー無しの persona) を避けるため facilitator を止める。
    let facilitator: ReturnType<typeof createFacilitator> | null = null;
    if (config.facilitator.enabled && llm && !isWorkerPool) {
      const facLogger = {
        debug: () => {},
        info: (meta: Record<string, unknown>, msg: string) => console.log(`  [facilitator] ${msg}`, meta),
        warn: (meta: Record<string, unknown>, msg: string) => console.warn(`  [facilitator] ${msg}`, meta),
        error: (meta: Record<string, unknown>, msg: string) => console.error(`  [facilitator] ${msg}`, meta),
      };
      facilitator = createFacilitator({
        core,
        llm,
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
        },
        // 収束トリガーは tick ごとに runtime-settings から読み、 チューニング UI の
        // 変更 (「20」等 / aufhebung-only ポリシー) を再起動なしで反映する。
        tuning: () => runtimeSettings.getFacilitatorTuning(),
        // 収束したらフォーラムポストを締める (gateway 起動後に forumFinalizer が結線される)。
        onConverged: (e) =>
          forumFinalizer?.({ scene: e.scene, summary: e.summary, title: e.title }),
      });
      facilitator.start();
      console.log(
        `  facilitator: started (tick=${config.facilitator.tickMs}ms, idleGap=${config.facilitator.idleGapMs}ms, maxPersonas=${config.facilitator.maxPersonas})`
      );
    }

    // 自動シード議論: 定期的にジャンル/ストアトレンドから headless 議論を立てる (#64/#65)。
    // 駆動は上の facilitator が担うので、 facilitator が動いている時だけ有効化する。
    // (worker-pool 時は facilitator を止めているので auto-seed も起動しない)
    let autoSeed: ReturnType<typeof createAutoSeedScheduler> | null = null;
    if (config.autoSeed.enabled && facilitator) {
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

    return { engine, bridge, core, peDb, facilitator, autoSeed };
  } catch (err) {
    console.warn("  persona-engine: startup failed:", err);
    return null;
  }
})();

const port = config.server.port;

// ─── S3 バックアチE�E: 月次自動スケジューラ起勁E(enabled かつ bucket 設定時のみ) ──
//   手動トリガ (slash /discutere-backup・npm run backup) は scheduler.trigger() を�E有、E
const backupScheduler = startBackupScheduler(config);

// 議論キューのサマリ生�E (slash /discutere-queue 用)。core は都度 open/close、E
function buildQueueText(): string {
  const core = createCore();
  try {
    const peDb = personaEngineLifecycle?.peDb ?? null;
    return formatQueueText(buildQueueSnapshot(core.client.raw, peDb, config.workspace));
  } finally {
    core.close();
  }
}

// WS 再設訁E Discord Gateway 常時接綁E(公閁EURL + 署名検証エンド�Eイント不要E、E
//   config.discord.botToken 未設定なめEskip 起動、E
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
  crawlDeps: {
    createCore: () => createCore(),
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
  classifyInboundMessage: createDiscordAutoDiscussionStarter({
    getLlm: () => autoDiscussionLlm,
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
    if (!handle || !config.discord.forum.enabled) return;
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
  process.exit(0);
};
process.on("SIGINT", () => gracefulShutdown("SIGINT"));
process.on("SIGTERM", () => gracefulShutdown("SIGTERM"));

serve({ fetch: app.fetch, port });
