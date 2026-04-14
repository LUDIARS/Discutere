import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { cors } from "hono/cors";
import { machinaRoutes } from "./machina/routes.js";
import { authRoutes, compositeAuthRoutes } from "./auth/routes.js";
import { userContext } from "./middleware/auth.js";

// Initialize DB (triggers schema creation)
import "./db/connection.js";

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

const port = parseInt(process.env.BACKEND_PORT || "3100", 10);

console.log(`Discutere listening on http://localhost:${port}`);
console.log(`  Auth:     /api/auth/{login-url,exchange,logout,me}`);
console.log(`  Tasks:    /api/groups/:id/tasks`);
console.log(`  Monitors: /api/groups/:id/monitors`);
console.log(`  Webhooks: /api/webhook/slack, /api/webhook/discord`);
console.log(`  Analyze:  /api/analyze`);

serve({ fetch: app.fetch, port });
