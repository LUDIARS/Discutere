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
  <h2>🧭 議論キュー <span class="muted" id="queue-totals">loading…</span>
    <button id="seed-sweep" style="float:right;padding:4px 10px;font-size:12px;">未検知から種まき</button>
  </h2>
  <div class="muted" id="seed-sweep-result"></div>
  <h3 style="margin:12px 0 4px;font-size:14px;">進行中の議論 session</h3>
  <table id="queue-sessions">
    <thead><tr><th>channel</th><th>guild</th><th>発話</th><th>発火</th><th>最終発話</th><th>操作</th></tr></thead>
    <tbody><tr><td colspan="6" class="muted">loading…</td></tr></tbody>
  </table>
  <h3 style="margin:12px 0 4px;font-size:14px;">未処理 DesignGap (仮説待ち)
    <button id="dismiss-all" class="bad" style="float:right;padding:4px 10px;font-size:12px;">未処理を一括却下</button>
  </h3>
  <table id="queue-gaps">
    <thead><tr><th>title</th><th>status</th><th>操作</th></tr></thead>
    <tbody><tr><td colspan="3" class="muted">loading…</td></tr></tbody>
  </table>
  <div class="muted" id="dismiss-result"></div>
  <h3 style="margin:12px 0 4px;font-size:14px;">検証待ちの仮説</h3>
  <table id="queue-hyps">
    <thead><tr><th>statement</th><th>status</th></tr></thead>
    <tbody><tr><td colspan="2" class="muted">loading…</td></tr></tbody>
  </table>
</div>

<div class="card">
  <h2>🗂️ データソース <span class="muted" id="ds-totals">loading…</span></h2>
  <div style="margin:8px 0 12px;">
    <label>active KG</label>
    <select id="ds-kg-select" style="margin:0 8px;padding:4px 8px;"></select>
    <button id="ds-kg-apply" style="padding:4px 10px;font-size:12px;">この KG に切替 (再起動要)</button>
    <span class="muted" id="ds-kg-result"></span>
  </div>
  <div id="ds-badges" class="muted">loading…</div>
  <div id="ds-utterances" style="margin-top:12px;"></div>
</div>

<div class="card">
  <h2>🚨 エラー <span class="muted" id="errors-totals"></span>
    <button id="errors-clear" class="bad" style="float:right;padding:4px 10px;font-size:12px;">クリア</button>
  </h2>
  <table id="errors-table">
    <thead><tr><th>時刻</th><th>level</th><th>source</th><th>message</th></tr></thead>
    <tbody><tr><td colspan="4" class="muted">loading…</td></tr></tbody>
  </table>
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

async function fetchQueue() {
  const res = await fetch("/api/admin/queue", { credentials: "include" });
  if (!res.ok) throw new Error("queue http " + res.status);
  return res.json();
}

function renderQueue(q) {
  document.getElementById("queue-totals").textContent =
    \`session \${q.totals.activeSessions} / gap \${q.totals.openGaps} / 仮説 \${q.totals.pendingHypotheses}\`;
  const st = document.querySelector("#queue-sessions tbody");
  st.innerHTML = q.activeSessions.length
    ? q.activeSessions.map((s) => \`<tr>
        <td><code>\${escapeHtml(s.channelId || "?")}</code></td>
        <td class="muted">\${escapeHtml(s.guildId || "—")}</td>
        <td>\${s.utteranceCount}</td>
        <td>\${s.fireCount}</td>
        <td class="muted">\${s.lastUtteranceAt ? new Date(s.lastUtteranceAt).toLocaleTimeString() : "—"}</td>
        <td><button class="bad close-session" data-session-id="\${escapeHtml(s.sessionId)}" style="padding:3px 9px;font-size:12px;">閉じる</button>
          <button class="converge-session" data-session-id="\${escapeHtml(s.sessionId)}" style="padding:3px 9px;font-size:12px;margin-left:4px;">まとめて閉じる</button></td>
      </tr>\`).join("")
    : '<tr><td colspan="6" class="muted">進行中の議論なし</td></tr>';
  st.querySelectorAll(".close-session").forEach((b) => b.addEventListener("click", () => closeSession(b.dataset.sessionId)));
  st.querySelectorAll(".converge-session").forEach((b) => b.addEventListener("click", () => convergeSession(b.dataset.sessionId)));
  const gt = document.querySelector("#queue-gaps tbody");
  gt.innerHTML = q.openGaps.length
    ? q.openGaps.map((g) => \`<tr><td>\${escapeHtml(g.title)}</td><td class="muted">\${escapeHtml(g.status || "open")}</td><td><button class="bad dismiss-gap" data-gap-id="\${escapeHtml(g.id)}" style="padding:3px 9px;font-size:12px;">却下</button></td></tr>\`).join("")
    : '<tr><td colspan="3" class="muted">未処理 gap なし</td></tr>';
  gt.querySelectorAll(".dismiss-gap").forEach((b) => b.addEventListener("click", () => dismissGap(b.dataset.gapId)));
  const ht = document.querySelector("#queue-hyps tbody");
  ht.innerHTML = q.pendingHypotheses.length
    ? q.pendingHypotheses.map((h) => \`<tr><td>\${escapeHtml(h.statement)}</td><td class="muted">\${escapeHtml(h.status || "open")}</td></tr>\`).join("")
    : '<tr><td colspan="2" class="muted">検証待ち仮説なし</td></tr>';
}


async function fetchErrors() {
  const res = await fetch("/api/admin/errors?limit=100", { credentials: "include" });
  if (!res.ok) throw new Error("errors http " + res.status);
  return res.json();
}

function renderErrors(data) {
  const list = data.errors || [];
  document.getElementById("errors-totals").textContent = list.length + " 件";
  const tbody = document.querySelector("#errors-table tbody");
  if (!list.length) {
    tbody.innerHTML = '<tr><td colspan="4" class="muted">エラーなし</td></tr>';
    return;
  }
  tbody.innerHTML = list.map((e) => {
    const dt = new Date(e.ts).toLocaleTimeString();
    const cls = e.level === "error" ? "action-error" : "action-skip";
    return '<tr><td>' + dt + '</td><td class="' + cls + '">' + escapeHtml(e.level) +
      '</td><td>' + escapeHtml(e.source) + '</td><td>' + escapeHtml(e.message) + '</td></tr>';
  }).join("");
}

document.getElementById("errors-clear").addEventListener("click", async () => {
  if (!confirm("エラーバッファをクリアしますか?")) return;
  try {
    await fetch("/api/admin/errors/clear", { method: "POST", credentials: "include" });
    fetchErrors().then(renderErrors).catch(() => {});
  } catch (e) { /* ignore */ }
});

document.getElementById("seed-sweep").addEventListener("click", async () => {
  const btn = document.getElementById("seed-sweep");
  const el = document.getElementById("seed-sweep-result");
  btn.disabled = true;
  el.textContent = "スイープ中…";
  try {
    const res = await fetch("/api/admin/seed-sweep", { method: "POST", credentials: "include" });
    const j = await res.json();
    el.textContent = res.ok ? ("走査 " + (j.scanned ?? 0) + " 件 / 種まき " + (j.seeded ?? 0) + " 件") : ("失敗: " + (j.error || res.status));
  } catch (e) { el.textContent = "失敗: " + e.message; }
  finally { btn.disabled = false; }
  fetchQueue().then(renderQueue).catch(() => {});
});

async function fetchLearningTopics() {
  const res = await fetch("/api/admin/learning/topics?limit=20&opinionsPerTopic=3", { credentials: "include" });
  if (!res.ok) throw new Error("learning topics http " + res.status);
  return res.json();
}

function renderLearningTopics(snap) {
  document.getElementById("learning-totals").textContent =
    "topic " + snap.totals.topics + " / opinion " + snap.totals.opinions + " / utterance " + snap.totals.utterances;
  const tbody = document.querySelector("#learning-topics tbody");
  if (!snap.topics || snap.topics.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" class="muted">no learning topics</td></tr>';
    return;
  }
  tbody.innerHTML = snap.topics.map((topic) =>
    '<tr>' +
      '<td>' + escapeHtml(topic.title) + '</td>' +
      '<td><span class="stat">' + topic.size + '</span></td>' +
      '<td>' + topic.opinionCount + '</td>' +
      '<td>' + topic.utteranceCount + '</td>' +
      '<td class="muted">' + escapeHtml(topic.status || "open") + '</td>' +
    '</tr>'
  ).join("");
}

// ─── データソース面 (§13: 閲覧 + 除外 + active KG 指定) ───
async function fetchDatasources() {
  const res = await fetch("/api/admin/datasources", { credentials: "include" });
  if (!res.ok) throw new Error("datasources http " + res.status);
  return res.json();
}

function renderDatasources(d) {
  const sources = d.sources || [];
  const kgs = d.knowledgeGraphs || [];
  document.getElementById("ds-totals").textContent =
    "active KG: " + esc(d.activeKg) + " / source " + sources.length + "種";
  // KG セレクタ (現在の active を強調)。
  const sel = document.getElementById("ds-kg-select");
  if (kgs.length) {
    sel.innerHTML = kgs.map((k) =>
      '<option value="' + esc(k.id) + '"' + (k.active ? " selected" : "") + '>' +
      esc(k.label || k.id) + (k.active ? " (現在)" : "") + '</option>').join("");
  } else {
    sel.innerHTML = '<option value="">(KG 未宣言)</option>';
  }
  // source 別件数バッジ + 中身を見るボタン。
  document.getElementById("ds-badges").innerHTML = sources.length
    ? sources.map((s) =>
        '<span class="stat" style="cursor:pointer;" data-ds-source="' + esc(s.source) + '">' +
        esc(s.source) + ': ' + s.count + '件' +
        (s.gameSlugs && s.gameSlugs.length ? ' <span class="muted">(' + esc(s.gameSlugs.join(", ")) + ')</span>' : '') +
        ' — 中身を見る</span>').join(" ")
    : '<span class="muted">取り込み済みデータソースなし</span>';
  document.querySelectorAll("[data-ds-source]").forEach((b) =>
    b.addEventListener("click", () => loadSourceUtterances(b.dataset.dsSource)));
}

async function loadSourceUtterances(source) {
  const box = document.getElementById("ds-utterances");
  box.innerHTML = '<div class="muted">読み込み中…</div>';
  try {
    const res = await fetch("/api/admin/datasources/" + encodeURIComponent(source) + "/utterances?limit=100", { credentials: "include" });
    if (!res.ok) throw new Error("http " + res.status);
    const d = await res.json();
    const items = d.items || [];
    if (!items.length) { box.innerHTML = '<div class="muted">発話なし</div>'; return; }
    box.innerHTML = '<h3 style="font-size:14px;margin:8px 0 4px;">' + esc(source) + ' の取り込み発話</h3>' +
      '<table><thead><tr><th>論者</th><th>本文</th><th>出所</th><th>操作</th></tr></thead><tbody>' +
      items.map((u) => {
        const link = u.sourceUrl
          ? '<a href="' + esc(u.sourceUrl) + '" target="_blank" rel="noopener">[' + esc(u.source) + ' ↗]</a>'
          : '<span class="muted">' + esc(u.source) + '</span>';
        const btn = u.excluded
          ? '<span class="muted">除外済</span>'
          : '<button class="bad ds-excl" data-uid="' + esc(u.utteranceId) + '" style="padding:3px 9px;font-size:12px;">除外</button>';
        return '<tr' + (u.excluded ? ' style="opacity:.5;"' : '') + '><td>' + esc(u.speaker) + '</td>' +
          '<td>' + esc(u.excerpt) + '</td><td>' + link + '</td><td>' + btn + '</td></tr>';
      }).join("") + '</tbody></table>';
    box.querySelectorAll(".ds-excl").forEach((b) =>
      b.addEventListener("click", () => excludeUtterance(b.dataset.uid, source)));
  } catch (err) {
    box.innerHTML = '<div class="muted">' + esc(err.message) + '</div>';
  }
}

async function excludeUtterance(utteranceId, source) {
  if (!confirm("この発話を除外しますか?(学習・議論から外れ、 再取り込みも防ぎます)")) return;
  try {
    await fetch("/api/admin/datasources/exclude", {
      method: "POST", headers: { "Content-Type": "application/json" }, credentials: "include",
      body: JSON.stringify({ utteranceId }),
    });
    loadSourceUtterances(source);
  } catch (e) { /* ignore */ }
}

document.getElementById("ds-kg-apply").addEventListener("click", async () => {
  const id = document.getElementById("ds-kg-select").value;
  const el = document.getElementById("ds-kg-result");
  if (!id) { el.textContent = "KG が宣言されていません"; return; }
  try {
    const res = await fetch("/api/admin/datasources/active-kg", {
      method: "POST", headers: { "Content-Type": "application/json" }, credentials: "include",
      body: JSON.stringify({ id }),
    });
    const j = await res.json();
    el.textContent = res.ok ? "✅ 次回起動から「" + id + "」 (再起動が必要)" : ("失敗: " + (j.error || res.status));
  } catch (e) { el.textContent = "失敗: " + e.message; }
});

async function refresh() {
  try {
    renderStatus(await fetchStatus());
    fetchMetrics("personas").then((r) => renderPersonaMetrics(r.metrics)).catch(() => {});
    fetchMetrics("rules").then((r) => renderRuleMetrics(r.metrics)).catch(() => {});
    fetchQueue().then(renderQueue).catch(() => {});
    fetchDatasources().then(renderDatasources).catch(() => {});
    fetchErrors().then(renderErrors).catch(() => {});
    fetchLearningTopics().then(renderLearningTopics).catch(() => {});
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
async function dismissGap(id) {
  if (!id || !confirm("この議論を却下しますか?(データ表示・学習から外れます)")) return;
  const el = document.getElementById("dismiss-result");
  try {
    const res = await fetch("/api/admin/gaps/" + encodeURIComponent(id) + "/dismiss", { method: "POST", credentials: "include" });
    const j = await res.json();
    el.textContent = res.ok ? ("却下 " + (j.dismissed ?? 0) + " 件") : ("失敗: " + (j.error || res.status));
  } catch (e) { el.textContent = "失敗: " + e.message; }
  fetchQueue().then(renderQueue).catch(() => {});
}

async function dismissAllOpen() {
  if (!confirm("未処理の議論を全て却下しますか?(データ表示・学習から外れます)")) return;
  const el = document.getElementById("dismiss-result");
  try {
    const res = await fetch("/api/admin/gaps/dismiss-open", { method: "POST", credentials: "include" });
    const j = await res.json();
    el.textContent = res.ok ? ("一括却下 " + (j.dismissed ?? 0) + " 件") : ("失敗: " + (j.error || res.status));
  } catch (e) { el.textContent = "失敗: " + e.message; }
  fetchQueue().then(renderQueue).catch(() => {});
}

async function closeSession(id) {
  if (!id || !confirm("この進行中の議論を閉じますか?(結論として終了し、AI の発言を止めます)")) return;
  const el = document.getElementById("dismiss-result");
  try {
    const res = await fetch("/api/admin/sessions/" + encodeURIComponent(id) + "/close", { method: "POST", credentials: "include" });
    const j = await res.json();
    el.textContent = res.ok ? ("閉じた session " + (j.endedSessions ?? 0) + " / gap " + (j.closedGaps ?? 0)) : ("失敗: " + (j.error || res.status));
  } catch (e) { el.textContent = "失敗: " + e.message; }
  fetchQueue().then(renderQueue).catch(() => {});
}

async function convergeSession(id) {
  if (!id || !confirm("この議論を収束させてまとめを生成し、閉じますか?(AIがまとめ文を作成して投稿します)")) return;
  const el = document.getElementById("dismiss-result");
  el.textContent = "収束中… (まとめ生成)";
  try {
    const res = await fetch("/api/admin/sessions/" + encodeURIComponent(id) + "/converge", { method: "POST", credentials: "include" });
    const j = await res.json();
    el.textContent = res.ok ? ("収束 " + (j.converged ?? 0) + " 件") : ("失敗: " + (j.error || res.status));
  } catch (e) { el.textContent = "失敗: " + e.message; }
  fetchQueue().then(renderQueue).catch(() => {});
}

document.getElementById("dismiss-all").addEventListener("click", dismissAllOpen);

setInterval(refresh, 5000);
</script>
</body>
</html>`;
