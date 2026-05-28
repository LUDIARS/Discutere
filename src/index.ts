import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { cors } from "hono/cors";
import { machinaRoutes } from "./machina/routes.js";
import { userContext } from "./middleware/auth.js";

// Initialize DB (triggers schema creation)
import "./db/connection.js";

const app = new Hono();

// CORS は当面残す (REST admin の X-User-Id ヘッダー受付用).
// Di-1 で Discord Interactions endpoint に移行したら不要になる.
app.use(
  "*",
  cors({
    origin: "*",
    allowMethods: ["GET", "POST", "PUT", "DELETE", "OPTIONS"],
    allowHeaders: ["Content-Type", "Authorization", "X-User-Id", "X-User-Role"],
  }),
);

// Health check
app.get("/health", (c) => c.json({ status: "ok", service: "discutere" }));

// ─── ユーザコンテキスト (X-User-Id / X-User-Role ヘッダーを読むだけ) ─────
app.use("/api/*", userContext());

// ─── MACHINA routes ──────────────────────────────────────────
app.route("/api", machinaRoutes);

const port = parseInt(process.env.BACKEND_PORT || "3100", 10);

console.log(`Discutere listening on http://localhost:${port}`);
console.log(`  Tasks:    /api/groups/:id/tasks`);
console.log(`  Monitors: /api/groups/:id/monitors`);
console.log(`  Webhooks: /api/webhook/slack, /api/webhook/discord`);
console.log(`  Analyze:  /api/analyze`);

serve({ fetch: app.fetch, port });
