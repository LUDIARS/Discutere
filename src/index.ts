import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { cors } from "hono/cors";
import Database from "better-sqlite3";
import path from "node:path";
import { machinaRoutes } from "./machina/routes.js";
import { authRoutes, compositeAuthRoutes } from "./auth/routes.js";
import { userContext } from "./middleware/auth.js";
import { discordRoutes } from "./api/discord-routes.js";
import { adminRoutes, setPersonaEngine } from "./api/admin-routes.js";
import { dashboardRoutes } from "./api/dashboard-routes.js";
import { assertProductionJwtSecret } from "./auth/jwt-guard.js";
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

// Initialize DB (triggers schema creation)
import "./db/connection.js";

// PR-C: JWT_SECRET production guard (本番で default secret なら即 throw)
assertProductionJwtSecret();

const app = new Hono();

const frontendUrl = process.env.FRONTEND_URL || "http://localhost:5174";

app.use("*", cors({
  origin: frontendUrl,
  credentials: true,
  allowMethods: ["GET", "POST", "PUT", "DELETE", "OPTIONS"],
  allowHeaders: ["Content-Type", "Authorization", "X-User-Id", "X-User-Role"],
}));

// Health check (認証不要)
app.get("/health", (c) => c.json({ status: "ok", service: "discutere" }));

// ─── Composite Auth (認証不要: ログイン前に呼ばれる) ───────────
app.route("/api/auth", compositeAuthRoutes);

// ─── 認証ミドルウェア (以降の /api/* に適用) ───────────────────
app.use("/api/*", userContext());

// ─── Auth Routes (認証必須: /me 等) ─────────────────────────
app.route("/api/auth", authRoutes);

// ─── MACHINA routes ──────────────────────────────────────────
app.route("/api", machinaRoutes);

// ─── Discord Interactions (PR-B: Ed25519 verify-based) ───────
app.route("/api", discordRoutes);

// ─── Admin (PR-B: 人間 → AI 介入経路) ────────────────────────
app.route("/api", adminRoutes);

// ─── Admin dashboard HTML (PR-H: 観察 + kill switch GUI) ────
app.route("/api", dashboardRoutes);

// PR-C: mode-state TTL cleanup を 15 min interval で起動 (24h 経過 session を回収)
const stopSessionCleanup = startSessionCleanup();

// PR-C / PR-I: persona-engine 起動 wiring
//   LLM backend は env LLM_BACKEND で切替: "anthropic" (= ANTHROPIC_API_KEY)
//   または "claude-cli" (= Lictor 経由 spawn、 環境に claude CLI が必要)。
const personaEngineLifecycle = (() => {
  const backend = (process.env.LLM_BACKEND ?? "anthropic").toLowerCase();
  let llm: LLMClient | null = null;
  if (backend === "claude-cli") {
    llm = new ClaudeCliClient({
      defaultTimeoutMs: Number(process.env.CLAUDE_CLI_TIMEOUT_MS ?? 120_000),
    });
    console.log("  persona-engine LLM: ClaudeCliClient (Lictor 経由 spawn)");
  } else if (process.env.ANTHROPIC_API_KEY) {
    llm = new AnthropicSdkClient();
    console.log("  persona-engine LLM: AnthropicSdkClient (HTTP)");
  } else {
    console.log("  persona-engine: skipped (set LLM_BACKEND=claude-cli or ANTHROPIC_API_KEY)");
    return null;
  }

  try {
    const peDbPath = process.env.DISCUTERE_PERSONA_ENGINE_DB ?? path.resolve("./data/persona-engine.db");
    const workspaceId = process.env.DISCATIER_WORKSPACE ?? "knowledge";
    const peDb = new Database(peDbPath);
    const core = createCore();
    const adapter = createDiscatierContextProvider(core, {
      // PR-I: AI 発話 / hypothesis を Discord channel に bot post
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
        // hypothesis は対応する gap session の scene が discord 由来でない場合スキップ
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
      maxFiresPerSession: Number(process.env.PERSONA_ENGINE_MAX_FIRES_PER_SESSION ?? 20),
      maxFiresPerRulePerSession: Number(
        process.env.PERSONA_ENGINE_MAX_FIRES_PER_RULE ?? 5
      ),
      tickMs: Number(process.env.PERSONA_ENGINE_TICK_MS ?? 5000),
    });
    const bridge = createEventBridge(core, engine, {
      workspaceId,
      pollMs: Number(process.env.PERSONA_ENGINE_BRIDGE_POLL_MS ?? 2000),
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

const port = parseInt(process.env.BACKEND_PORT || "3100", 10);

console.log(`Discutere listening on http://localhost:${port}`);
console.log(`  Auth:     /api/auth/{login-url,exchange,logout,me}`);
console.log(`  Tasks:    /api/groups/:id/tasks`);
console.log(`  Monitors: /api/groups/:id/monitors`);
console.log(`  Webhooks: /api/webhook/slack, /api/webhook/discord`);
console.log(`  Discord:  /api/discord/interactions`);
console.log(`  Admin:    /api/admin/{rules/enabled,session/reset,status}`);
console.log(`  Dashboard: /api/admin/dashboard (HTML, admin role)`);
console.log(`  Analyze:  /api/analyze`);

serve({ fetch: app.fetch, port });
