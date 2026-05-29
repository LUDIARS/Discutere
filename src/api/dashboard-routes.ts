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
  <h2>Persona metrics (proposed / integrated / rejected)</h2>
  <table id="personas">
    <thead><tr><th>persona</th><th>proposed</th><th>integrated</th><th>rejected</th><th>acceptance</th></tr></thead>
    <tbody><tr><td colspan="5" class="muted">loading…</td></tr></tbody>
  </table>
</div>

<div class="card">
  <h2>Rule metrics (fires / skips / errors)</h2>
  <table id="rules">
    <thead><tr><th>rule</th><th>fires</th><th>skips</th><th>errors</th></tr></thead>
    <tbody><tr><td colspan="4" class="muted">loading…</td></tr></tbody>
  </table>
</div>

<div class="card">
  <h2>🔧 Rule Viewer (議論エンジンの rule 群 + AI 自己補正履歴)</h2>
  <div style="margin-bottom: 8px;">
    <label><input type="checkbox" id="rules-show-removed"> include removed</label>
    <button id="rules-refresh" style="margin-left: 8px;">refresh</button>
  </div>
  <table id="rules-table">
    <thead><tr>
      <th>id</th><th>type</th><th>target</th><th>cooldown</th>
      <th>fired</th><th>added by</th><th>status</th>
    </tr></thead>
    <tbody><tr><td colspan="7" class="muted">loading…</td></tr></tbody>
  </table>
  <div id="rule-detail-card" class="muted" style="margin-top: 12px;">行をクリックで rule 詳細 + 補正履歴</div>
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

async function fetchMetrics(kind) {
  const res = await fetch("/api/admin/metrics/" + kind, { credentials: "include" });
  if (!res.ok) throw new Error(kind + " metrics http " + res.status);
  return res.json();
}

function renderPersonaMetrics(list) {
  const tbody = document.querySelector("#personas tbody");
  if (!list || list.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" class="muted">no personas</td></tr>';
    return;
  }
  const sorted = [...list].sort((a, b) => (b.proposed || 0) - (a.proposed || 0));
  tbody.innerHTML = sorted.map((m) => {
    const rate = m.acceptanceRate === null ? "—" : (m.acceptanceRate * 100).toFixed(0) + "%";
    return \`<tr>
      <td>\${escapeHtml(m.name)} <span class="muted">(\${escapeHtml(m.display_name)})</span></td>
      <td>\${m.proposed}</td>
      <td>\${m.integrated}</td>
      <td>\${m.rejected}</td>
      <td>\${rate}</td>
    </tr>\`;
  }).join("");
}

function renderRuleMetrics(list) {
  const tbody = document.querySelector("#rules tbody");
  if (!list || list.length === 0) {
    tbody.innerHTML = '<tr><td colspan="4" class="muted">no rule activity</td></tr>';
    return;
  }
  tbody.innerHTML = list.map((m) => \`<tr>
    <td>\${escapeHtml(m.id)}</td>
    <td>\${m.fires}</td>
    <td>\${m.skips}</td>
    <td>\${m.errors}</td>
  </tr>\`).join("");
}

async function fetchRules(includeRemoved) {
  const url = "/api/admin/rules" + (includeRemoved ? "?includeRemoved=1" : "");
  const res = await fetch(url, { credentials: "include" });
  if (!res.ok) throw new Error("rules http " + res.status);
  return res.json();
}

async function fetchRuleLogs(id) {
  const res = await fetch("/api/admin/rules/" + encodeURIComponent(id) + "/logs?limit=30", { credentials: "include" });
  if (!res.ok) throw new Error("rule logs http " + res.status);
  return res.json();
}

function renderRulesTable(rules) {
  const tbody = document.querySelector("#rules-table tbody");
  if (!rules || rules.length === 0) {
    tbody.innerHTML = '<tr><td colspan="7" class="muted">no rules</td></tr>';
    return;
  }
  tbody.innerHTML = rules.map((r) => {
    const trigger = r.trigger_type + (r.trigger_type === "tick"
      ? " (" + (r.tick_sec || "?") + "s)"
      : " (" + (r.event_kind || "*") + ")");
    const status = !r.enabled
      ? '<span class="stat bad">disabled</span>'
      : r.is_ai_added
        ? '<span class="stat warn">AI-added</span>'
        : '<span class="stat">seed</span>';
    const removed = r.removed_at ? '<div class="muted">removed by ' + escapeHtml(r.removed_by || "?") + ': ' + escapeHtml(r.removed_reason || "") + '</div>' : "";
    const lastFired = r.last_fired_at
      ? new Date(r.last_fired_at * 1000).toLocaleTimeString()
      : '<span class="muted">never</span>';
    return \`<tr data-rule-id="\${escapeHtml(r.id)}" style="cursor: pointer;">
      <td><code>\${escapeHtml(r.id)}</code>\${removed}</td>
      <td>\${escapeHtml(trigger)}</td>
      <td>\${escapeHtml(r.target || "—")}</td>
      <td>\${r.cooldown_sec}s</td>
      <td>\${lastFired}</td>
      <td class="muted">\${escapeHtml(r.added_by)}</td>
      <td>\${status}</td>
    </tr>\`;
  }).join("");
  tbody.querySelectorAll("tr[data-rule-id]").forEach((tr) => {
    tr.addEventListener("click", () => showRuleDetail(tr.dataset.ruleId));
  });
}

async function showRuleDetail(id) {
  const card = document.getElementById("rule-detail-card");
  card.innerHTML = '<em class="muted">loading…</em>';
  try {
    const data = await fetchRuleLogs(id);
    if (!data.logs || data.logs.length === 0) {
      card.innerHTML = '<strong>' + escapeHtml(id) + '</strong> — no logs yet';
      return;
    }
    const rows = data.logs.map((l) => {
      const dt = new Date(l.ts * 1000).toLocaleString();
      return \`<tr>
        <td>\${dt}</td>
        <td class="action-\${l.action}">\${escapeHtml(l.action)}</td>
        <td>\${escapeHtml(l.actor)}</td>
        <td>\${escapeHtml(l.detail || "")}</td>
      </tr>\`;
    }).join("");
    card.innerHTML = \`<strong>\${escapeHtml(id)}</strong> — last \${data.logs.length} logs
      <table style="margin-top: 8px;">
        <thead><tr><th>time</th><th>action</th><th>actor</th><th>detail</th></tr></thead>
        <tbody>\${rows}</tbody>
      </table>\`;
  } catch (err) {
    card.innerHTML = '<span class="stat bad">' + escapeHtml(err.message) + '</span>';
  }
}

async function refreshRules() {
  const showRemoved = document.getElementById("rules-show-removed").checked;
  try {
    const r = await fetchRules(showRemoved);
    renderRulesTable(r.rules);
  } catch (err) {
    document.querySelector("#rules-table tbody").innerHTML =
      '<tr><td colspan="7" class="muted">' + escapeHtml(err.message) + '</td></tr>';
  }
}
document.getElementById("rules-refresh").addEventListener("click", refreshRules);
document.getElementById("rules-show-removed").addEventListener("change", refreshRules);

async function refresh() {
  try {
    renderStatus(await fetchStatus());
    fetchMetrics("personas").then((r) => renderPersonaMetrics(r.metrics)).catch(() => {});
    fetchMetrics("rules").then((r) => renderRuleMetrics(r.metrics)).catch(() => {});
    refreshRules();
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
