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
} from "./persona-engine/index.js";
import { postDiscussionToDiscord } from "./discord-hook/discussion-bridge.js";
import { createDiscordAutoDiscussionStarter } from "./discord-hook/auto-discussion.js";
import { startDiscordGateway } from "./discord-hook/gateway.js";
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

// PR-C: mode-state TTL cleanup めE15 min interval で起勁E(24h 経過 session を回叁E
const stopSessionCleanup = startSessionCleanup();

let autoDiscussionLlm: LLMClient | null = null;

// PR-C / PR-I: persona-engine 起勁Ewiring
//   LLM backend は config.llm.backend で刁E��: "anthropic" (= anthropicApiKey)
//   また�E "claude-cli" (= Lictor 経由 spawn、E環墁E�� claude CLI が忁E��E、E
const personaEngineLifecycle = (() => {
  let llm: LLMClient | null = null;
  if (config.llm.backend === "claude-cli") {
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
    const peDbPath = config.personaEngine.dbPath;
    const workspaceId = config.workspace;
    const peDb = new Database(peDbPath);
    const core = createCore();
    const adapter = createDiscatierContextProvider(core, {
      // PR-I: AI 発話 / hypothesis めEDiscord channel に bot post
      async onPostedUtterance(input) {
        const r = await postDiscussionToDiscord({
          core,
          workspaceId: input.workspaceId,
          sessionId: input.sessionId,
          kind: "utterance",
          speakerLabel: `persona:${input.byPersonaId}`,
          text: input.text,
        });
        if (!r.ok) {
          console.warn("  persona-engine: discord utterance post skipped:", r.reason);
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
        const r = await postDiscussionToDiscord({
          core,
          workspaceId: input.workspaceId,
          sessionId: session.id,
          kind: "hypothesis",
          speakerLabel: `persona:${input.byPersonaId}`,
          text: input.statement,
        });
        if (!r.ok) {
          console.warn("  persona-engine: discord hypothesis post skipped:", r.reason);
        }
      },
    });
    const engine = createPersonaEngine({
      db: peDb,
      llm,
      contextProvider: adapter,
      workspaceId,
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
    return { engine, bridge, core, peDb };
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
  crawlDeps: {
    createCore: () => createCore(),
    workspaceId: config.workspace,
    youtubeApiKey: process.env.DISCUTERE_YOUTUBE_API_KEY ?? null,
  },
  getEngine: () => getPersonaEngine(),
  buildQueueText,
  triggerBackup: () => backupScheduler.trigger(),
  classifyInboundMessage: createDiscordAutoDiscussionStarter({
    getLlm: () => autoDiscussionLlm,
  }),
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

serve({ fetch: app.fetch, port });
