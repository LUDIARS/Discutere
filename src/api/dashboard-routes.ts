/**
 * Admin dashboard (PR-H / MISSING #4-1 + #4-2).
 *
 * - GET /admin/dashboard — server-rendered HTML 1 page (vanilla JS で polling)
 *   * persona / rule 数表示
 *   * runtime kill switch toggle (POST /api/admin/rules/enabled)
 *   * 直近 rule_log tail (= 議論の自走状況リアルタイム)
 *
 * Discord-only pivot を維持しつつ、 frontend なしで「人間が議論を観察 / 介入」
 * できる最小 UI。 admin role 必須 (= 既存 admin-routes と同じ guard を流用)。
 *
 * このページから admin API を fetch するので、 cookie 経由認証で同一 origin 前提。
 */

import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";

export const dashboardRoutes = new Hono();

dashboardRoutes.get("/admin/dashboard", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  return c.html(HTML);
});

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}

const HTML = `<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<title>Discutere — persona-engine dashboard</title>
<style>
  :root { color-scheme: light dark; }
  body { font-family: system-ui, -apple-system, "Segoe UI", "Hiragino Kaku Gothic ProN", sans-serif; max-width: 1100px; margin: 24px auto; padding: 0 16px; line-height: 1.5; }
  h1 { border-bottom: 2px solid #4a90e2; padding-bottom: 8px; }
  .card { border: 1px solid #ccc4; border-radius: 8px; padding: 16px; margin: 16px 0; background: #fff1; }
  .stat { display: inline-block; padding: 4px 12px; margin: 4px; border-radius: 6px; background: #4a90e21a; font-weight: bold; }
  .stat.warn { background: #f5a52333; }
  .stat.bad { background: #e2484833; }
  button { padding: 8px 16px; border-radius: 6px; border: 1px solid #4a90e2; background: #4a90e2; color: white; cursor: pointer; font-size: 14px; }
  button.bad { background: #e24848; border-color: #e24848; }
  button:disabled { opacity: 0.5; cursor: wait; }
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th, td { padding: 6px 10px; text-align: left; border-bottom: 1px solid #ccc4; }
  th { background: #4a90e21a; }
  td.action-fire { color: #2e7d32; font-weight: bold; }
  td.action-skip { color: #888; }
  td.action-error { color: #c62828; font-weight: bold; }
  .muted { color: #888; font-size: 12px; }
  .refresh { float: right; font-size: 12px; }
</style>
</head>
<body>
<h1>Discutere persona-engine dashboard <span class="refresh muted" id="refresh">—</span></h1>

<div class="card">
  <h2>Engine status</h2>
  <div id="status-stats">loading…</div>
  <div style="margin-top: 12px;">
    <button id="toggle-rules" disabled>loading…</button>
    <span class="muted" id="toggle-hint"></span>
  </div>
</div>

<div class="card">
  <h2>Recent rule log (latest 10)</h2>
  <table id="logs">
    <thead><tr><th>time</th><th>rule</th><th>actor</th><th>action</th><th>detail</th></tr></thead>
    <tbody><tr><td colspan="5" class="muted">loading…</td></tr></tbody>
  </table>
</div>

<div class="card">
  <h2>Session ops</h2>
  <p class="muted">特定 session の safety cap を解放したい時:</p>
  <form id="reset-form">
    <input type="text" id="reset-sid" placeholder="sessionId" style="padding: 6px; width: 300px;" required>
    <button type="submit">reset</button>
    <span id="reset-result" class="muted"></span>
  </form>
</div>

<script>
async function fetchStatus() {
  const res = await fetch("/api/admin/status", { credentials: "include" });
  if (!res.ok) throw new Error("status http " + res.status);
  return res.json();
}

async function setRulesEnabled(enabled) {
  const res = await fetch("/api/admin/rules/enabled", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    credentials: "include",
    body: JSON.stringify({ enabled }),
  });
  if (!res.ok) throw new Error("toggle http " + res.status);
  return res.json();
}

async function resetSession(sid) {
  const res = await fetch("/api/admin/session/reset", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    credentials: "include",
    body: JSON.stringify({ sessionId: sid }),
  });
  return { ok: res.ok, status: res.status, body: await res.json().catch(() => ({})) };
}

function renderStatus(s) {
  const stats = document.getElementById("status-stats");
  const attached = s.persona_engine_attached;
  const enabled = s.rules_runtime_enabled;
  stats.innerHTML = [
    \`<span class="stat \${attached ? "" : "bad"}">engine \${attached ? "attached" : "DETACHED"}</span>\`,
    \`<span class="stat \${enabled ? "" : "warn"}">rules \${enabled ? "ENABLED" : "DISABLED"}</span>\`,
    \`<span class="stat">personas: \${s.persona_count}</span>\`,
    \`<span class="stat">active rules: \${s.rule_count}</span>\`,
  ].join("");

  const btn = document.getElementById("toggle-rules");
  btn.disabled = false;
  if (enabled) {
    btn.textContent = "DISABLE rules (kill switch)";
    btn.className = "bad";
  } else {
    btn.textContent = "ENABLE rules";
    btn.className = "";
  }
  document.getElementById("toggle-hint").textContent =
    "engine.setRulesEnabled() を runtime で叩く。 再起動なしで全 rule fire を止める/再開。";

  const tbody = document.querySelector("#logs tbody");
  if (!s.recent_rule_logs || s.recent_rule_logs.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" class="muted">no recent logs</td></tr>';
  } else {
    tbody.innerHTML = s.recent_rule_logs.map((l) => {
      const dt = new Date(l.ts * 1000).toLocaleTimeString();
      return \`<tr>
        <td>\${dt}</td>
        <td>\${escapeHtml(l.rule_id || "—")}</td>
        <td>\${escapeHtml(l.actor)}</td>
        <td class="action-\${l.action}">\${escapeHtml(l.action)}</td>
        <td>\${escapeHtml(l.detail || "")}</td>
      </tr>\`;
    }).join("");
  }

  document.getElementById("refresh").textContent = "last refresh: " + new Date().toLocaleTimeString();
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#039;"}[c]));
}

async function refresh() {
  try {
    renderStatus(await fetchStatus());
  } catch (err) {
    document.getElementById("status-stats").innerHTML = '<span class="stat bad">' + escapeHtml(err.message) + '</span>';
  }
}

document.getElementById("toggle-rules").addEventListener("click", async () => {
  const btn = document.getElementById("toggle-rules");
  btn.disabled = true;
  try {
    const current = await fetchStatus();
    await setRulesEnabled(!current.rules_runtime_enabled);
    await refresh();
  } catch (err) {
    alert("toggle failed: " + err.message);
  } finally {
    btn.disabled = false;
  }
});

document.getElementById("reset-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const sid = document.getElementById("reset-sid").value.trim();
  const result = document.getElementById("reset-result");
  if (!sid) return;
  const r = await resetSession(sid);
  result.textContent = r.ok ? "✅ reset" : "⚠️ " + (r.body.error || r.status);
  await refresh();
});

refresh();
setInterval(refresh, 5000);
</script>
</body>
</html>`;
