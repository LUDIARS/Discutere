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
import { PersonasRepo } from "./persona-engine/db/personas-repo.js";
import { createFacilitator } from "./persona-engine/facilitator/index.js";
import { postDiscussionToDiscord } from "./discord-hook/discussion-bridge.js";
import { createDiscordAutoDiscussionStarter } from "./discord-hook/auto-discussion.js";
import { startDiscordGateway } from "./discord-hook/gateway.js";
import { ensureReactionTables, recordPostedMessage, applyReaction } from "./discord-hook/reactions.js";
import { queueRoutes } from "./api/queue-routes.js";
import { buildQueueSnapshot, formatQueueText } from "./queue/snapshot.js";
import { startBackupScheduler } from "./backup/runner.js";
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

// PR-C: mode-state TTL cleanup めE15 min interval で起勁E(24h 経過 session を回叁E
const stopSessionCleanup = startSessionCleanup();

let autoDiscussionLlm: LLMClient | null = null;
// backend=worker-pool 時に握る常駐ワーカープール (shutdown で stop する)。
let workerPool: WorkerPool | null = null;
// worker-pool 時の 8 ペルソナ seed (persona id = worker id)。
let workerPersonaSeeds: PersonaSeed[] | undefined;
const isWorkerPool = config.llm.backend === "worker-pool";

// PR-C / PR-I: persona-engine 起勁Ewiring
//   LLM backend は config.llm.backend で刁E��: "anthropic" (= anthropicApiKey)
//   また�E "claude-cli" (= Lictor 経由 spawn、E環墁E�� claude CLI が忁E��E、E
//   または "worker-pool" (= サブスク Lictor 常駐ワーカー、 spec/feature/persistent-worker-pool.md)
const personaEngineLifecycle = (() => {
  let llm: LLMClient | null = null;
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
    // persona の人間名と、 webhook 投稿の要否を解決する。
    // facilitator (進行役) は Discutere bot として直接、 議論 persona は webhook で人間名。
    const resolveSpeaker = (byPersonaId: string) => {
      const p = relayPersonas.get(byPersonaId);
      return {
        name: p?.display_name ?? byPersonaId,
        viaWebhook: byPersonaId !== "facilitator",
      };
    };
    const adapter = createDiscatierContextProvider(core, {
      // PR-I: AI 発話 / hypothesis を Discord channel に post
      async onPostedUtterance(input) {
        const sp = resolveSpeaker(input.byPersonaId);
        const r = await postDiscussionToDiscord({
          core,
          workspaceId: input.workspaceId,
          sessionId: input.sessionId,
          kind: "utterance",
          speakerLabel: sp.name,
          viaWebhook: sp.viaWebhook,
          text: input.text,
        });
        if (!r.ok) {
          console.warn("  persona-engine: discord utterance post skipped:", r.reason);
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
      ruleSeeds: isWorkerPool ? DEBATE_RULE_SEEDS : undefined,
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
      });
      facilitator.start();
      console.log(
        `  facilitator: started (tick=${config.facilitator.tickMs}ms, idleGap=${config.facilitator.idleGapMs}ms, maxPersonas=${config.facilitator.maxPersonas})`
      );
    }

    return { engine, bridge, core, peDb, facilitator };
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
void discordGatewayLifecycle;

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
