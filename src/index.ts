import { serve } from "@hono/node-server";
import { Hono } from "hono";
import { cors } from "hono/cors";
import { machinaRoutes } from "./machina/routes.js";

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

// Health check
app.get("/health", (c) => c.json({ status: "ok", service: "discutere" }));

// MACHINA routes
app.route("/api", machinaRoutes);

const port = parseInt(process.env.BACKEND_PORT || "3100", 10);

console.log(`Discutere listening on http://localhost:${port}`);
console.log(`  Tasks:    /api/workspaces/:id/tasks`);
console.log(`  Monitors: /api/workspaces/:id/monitors`);
console.log(`  Webhooks: /api/webhook/slack, /api/webhook/discord`);
console.log(`  Analyze:  /api/analyze`);

serve({ fetch: app.fetch, port });
