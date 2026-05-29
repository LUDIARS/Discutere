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
import { assertProductionJwtSecret } from "./auth/jwt-guard.js";
import { startSessionCleanup } from "./machina/mode-state.js";
import { createCore } from "./core/index.js";
import { createDiscatierContextProvider } from "./discatier-engine-adapter/index.js";
import { createEventBridge } from "./discatier-engine-adapter/event-bridge.js";
import {
  AnthropicSdkClient,
  createPersonaEngine,
} from "./persona-engine/index.js";

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

// PR-C: mode-state TTL cleanup を 15 min interval で起動 (24h 経過 session を回収)
const stopSessionCleanup = startSessionCleanup();

// PR-C: persona-engine 起動 wiring (ANTHROPIC_API_KEY あれば自動起動)
const personaEngineLifecycle = (() => {
  if (!process.env.ANTHROPIC_API_KEY) {
    console.log("  persona-engine: skipped (ANTHROPIC_API_KEY not set)");
    return null;
  }
  try {
    const peDbPath = process.env.DISCUTERE_PERSONA_ENGINE_DB ?? path.resolve("./data/persona-engine.db");
    const workspaceId = process.env.DISCATIER_WORKSPACE ?? "knowledge";
    const peDb = new Database(peDbPath);
    const core = createCore();
    const adapter = createDiscatierContextProvider(core, {});
    const engine = createPersonaEngine({
      db: peDb,
      llm: new AnthropicSdkClient(),
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
console.log(`  Analyze:  /api/analyze`);

serve({ fetch: app.fetch, port });
