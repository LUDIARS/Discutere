import { Hono } from "hono";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import {
  buildLearningLayerSnapshot,
  normalizeLearningLayer,
} from "../visualize/learning-layers.js";
import { listConclusions, getConclusionDetail } from "../visualize/conclusions.js";

export const learningViewRoutes = new Hono();

learningViewRoutes.get("/learning", (c) => c.html(HTML));

// 収束した議論の結論一覧 (#66 — Learning View 統合)
learningViewRoutes.get("/learning/conclusions", (c) => {
  const config = getConfig();
  const limit = Number(c.req.query("limit") ?? 100);
  const core = createCore();
  try {
    return c.json({ conclusions: listConclusions(core, config.workspace, limit) });
  } finally {
    core.close();
  }
});

// 1 件の結論の裏の論述データ (議論ログ / 止揚 / 高評価意見)
learningViewRoutes.get("/learning/conclusion", (c) => {
  const config = getConfig();
  const gapId = c.req.query("gap") ?? "";
  const core = createCore();
  try {
    const detail = getConclusionDetail(core, config.workspace, gapId);
    if (!detail) return c.json({ error: "not found" }, 404);
    return c.json(detail);
  } finally {
    core.close();
  }
});

learningViewRoutes.get("/learning/data", (c) => {
  const config = getConfig();
  const layer = normalizeLearningLayer(c.req.query("layer"));
  const limit = Number(c.req.query("limit") ?? 60);
  const detailLimit = Number(c.req.query("detailLimit") ?? c.req.query("opinionsPerTopic") ?? 8);
  const core = createCore();
  try {
    return c.json(
      buildLearningLayerSnapshot(core.client.raw, config.workspace, layer, {
        limit,
        detailLimit,
      })
    );
  } finally {
    core.close();
  }
});

const HTML = `<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Discutere Learning View</title>
<style>
  :root { color-scheme: light dark; --line:#d0d7de55; --muted:#6b7280; --accent:#2563eb; }
  body { margin: 0; font-family: system-ui, -apple-system, "Segoe UI", "Hiragino Kaku Gothic ProN", Meiryo, sans-serif; line-height: 1.5; }
  header { padding: 20px 24px 12px; border-bottom: 1px solid var(--line); }
  h1 { margin: 0 0 10px; font-size: 22px; }
  main { padding: 18px 24px 28px; display: grid; grid-template-columns: minmax(0, 1.1fr) minmax(340px, .9fr); gap: 18px; }
  button { border: 1px solid var(--line); border-radius: 6px; padding: 7px 12px; background: transparent; color: inherit; cursor: pointer; }
  button.active { border-color: var(--accent); background: color-mix(in srgb, var(--accent) 18%, transparent); font-weight: 700; }
  .tabs { display: flex; gap: 8px; flex-wrap: wrap; margin-bottom: 8px; }
  .muted { color: var(--muted); font-size: 12px; }
  .items { display: grid; gap: 10px; }
  .item { border: 1px solid var(--line); border-radius: 8px; padding: 12px; }
  .item-head { display: flex; justify-content: space-between; gap: 12px; align-items: baseline; }
  .item-title { font-weight: 700; overflow-wrap: anywhere; }
  .item-size { white-space: nowrap; }
  .details { margin: 8px 0 0; padding-left: 20px; }
  .details li { margin: 4px 0; overflow-wrap: anywhere; }
  .graph-wrap { position: sticky; top: 12px; }
  svg { width: 100%; height: 440px; border: 1px solid var(--line); border-radius: 8px; }
  svg text { fill: currentColor; font-size: 11px; }
  @media (max-width: 860px) { main { grid-template-columns: 1fr; } .graph-wrap { position: static; } svg { height: 320px; } }
</style>
</head>
<body>
<header>
  <h1>Discutere Learning View</h1>
  <div class="tabs" aria-label="learning layer">
    <button data-layer="knowledge" class="active">基礎知識</button>
    <button data-layer="games">ゲーム学習</button>
    <button data-layer="opinions">話題と意見</button>
    <button data-layer="conclusions">結論</button>
  </div>
  <div class="muted" id="totals">loading...</div>
</header>
<main>
  <section>
    <div class="items" id="items"><div class="muted">loading...</div></div>
  </section>
  <aside class="graph-wrap">
    <svg id="graph" viewBox="0 0 560 440" role="img" aria-label="学習データの大きさグラフ"></svg>
    <div class="muted" id="graph-help">円の大きさは層ごとの学習量スコアです。</div>
  </aside>
</main>
<script>
let currentLayer = "knowledge";

function esc(value) {
  return String(value ?? "").replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#039;"}[c]));
}

function label(value, max) {
  const text = String(value ?? "");
  return text.length <= max ? text : text.slice(0, max - 1) + "...";
}

async function load(layer) {
  if (layer === "conclusions") {
    const res = await fetch("/learning/conclusions?limit=200");
    if (!res.ok) throw new Error("conclusions http " + res.status);
    return res.json();
  }
  const res = await fetch("/learning/data?layer=" + encodeURIComponent(layer) + "&limit=80&detailLimit=8");
  if (!res.ok) throw new Error("learning data http " + res.status);
  return res.json();
}

function setActiveLayer(layer) {
  currentLayer = layer;
  document.querySelectorAll("button[data-layer]").forEach((button) => {
    button.classList.toggle("active", button.dataset.layer === layer);
  });
  document.getElementById("items").innerHTML = '<div class="muted">loading...</div>';
  document.getElementById("graph").innerHTML = "";
  load(layer).then(render).catch((err) => {
    document.getElementById("items").innerHTML = '<div class="muted">' + esc(err.message) + '</div>';
  });
}

function render(snap) {
  if (currentLayer === "conclusions") return renderConclusions(snap);
  if (snap.layer === "knowledge") return renderKnowledge(snap);
  if (snap.layer === "games") return renderGames(snap);
  return renderOpinions(snap);
}

function renderConclusions(snap) {
  const list = snap.conclusions || [];
  document.getElementById("totals").textContent = "結論: " + list.length + " 件 (収束した議論)";
  document.getElementById("graph-help").textContent = "円の大きさ = 議論の発話数。";
  document.getElementById("items").innerHTML = list.length
    ? list.map((c) =>
        '<article class="item" data-gap="' + esc(c.gapId) + '">' +
          '<div class="item-head">' +
            '<div class="item-title">' + esc(c.title) + '</div>' +
            '<div class="item-size muted">発話 ' + c.utteranceCount + ' / 止揚 ' + c.aufhebungCount + '</div>' +
          '</div>' +
          '<div style="margin-top:6px;">' + esc(c.conclusion || "(まとめ未生成)") + '</div>' +
          '<button class="detail-btn" data-gap="' + esc(c.gapId) + '" style="margin-top:8px;">論述データを見る</button>' +
          '<div class="detail-slot"></div>' +
        '</article>').join("")
    : '<div class="muted">まだ収束した議論はありません</div>';
  document.querySelectorAll(".detail-btn").forEach((btn) => {
    btn.addEventListener("click", () => loadConclusionDetail(btn));
  });
  renderGraph(list.map((c) => ({ title: c.title, size: c.utteranceCount, status: "closed" })), "結論なし");
}

async function loadConclusionDetail(btn) {
  const gap = btn.dataset.gap;
  const slot = btn.parentElement.querySelector(".detail-slot");
  slot.innerHTML = '<div class="muted">loading...</div>';
  try {
    const res = await fetch("/learning/conclusion?gap=" + encodeURIComponent(gap));
    if (!res.ok) throw new Error("detail http " + res.status);
    const d = await res.json();
    const auf = (d.aufhebungen || []).length
      ? '<div style="margin-top:8px;"><b>止揚ストック</b><ol class="details">' +
        d.aufhebungen.map((s) => '<li>' + esc(s) + '</li>').join("") + '</ol></div>' : "";
    const top = (d.topOpinions || []).length
      ? '<div style="margin-top:8px;"><b>高評価意見</b><ol class="details">' +
        d.topOpinions.map((o) => '<li>+' + o.score + ' ' + esc(o.speaker) + ': ' + esc(o.content) + '</li>').join("") + '</ol></div>' : "";
    const log = '<div style="margin-top:8px;"><b>議論ログ (' + (d.transcript || []).length + '発話)</b><ol class="details">' +
      (d.transcript || []).map((u) => '<li><span class="muted">[' + esc(u.speaker) + ']</span> ' + esc(u.content) + '</li>').join("") + '</ol></div>';
    slot.innerHTML = auf + top + log;
    btn.style.display = "none";
  } catch (err) {
    slot.innerHTML = '<div class="muted">' + esc(err.message) + '</div>';
  }
}

function renderKnowledge(snap) {
  document.getElementById("totals").textContent =
    "基礎知識: affect " + snap.totals.affects +
    " / game " + snap.totals.games +
    " / mechanic " + snap.totals.mechanics +
    " / aesthetic " + snap.totals.aesthetics +
    " / gap " + snap.totals.gaps +
    " / opinion " + snap.totals.hypotheses;
  document.getElementById("graph-help").textContent =
    "円の大きさ = affect語彙がMechanic/Affect/Gapで使われている数。";
  const affects = snap.affects || [];
  document.getElementById("items").innerHTML = affects.length
    ? affects.map((affect) => '<article class="item">' +
        '<div class="item-head">' +
          '<div class="item-title">' + esc(affect.key) + (affect.labelJa ? ' <span class="muted">' + esc(affect.labelJa) + '</span>' : '') + '</div>' +
          '<div class="item-size muted">usage ' + affect.usageCount + '</div>' +
        '</div>' +
        '<div class="muted">' + esc([affect.valence, affect.mda].filter(Boolean).join(" / ") || "uncategorized") + '</div>' +
        (affect.description ? '<div class="muted">' + esc(affect.description) + '</div>' : '') +
      '</article>').join("")
    : '<div class="muted">基礎知識データはまだありません</div>';
  renderGraph(affects.map((affect) => ({
    title: affect.key,
    size: affect.size,
    status: affect.valence,
  })), "affectなし");
}

function renderGames(snap) {
  document.getElementById("totals").textContent =
    "ゲーム学習: game " + snap.totals.games +
    " / mechanic " + snap.totals.mechanics +
    " / aesthetic " + snap.totals.aesthetics +
    " / opinion " + snap.totals.opinions;
  document.getElementById("graph-help").textContent =
    "円の大きさ = mechanic数 x 2 + aesthetic数 + opinion数 x 3。";
  const games = snap.games || [];
  document.getElementById("items").innerHTML = games.length
    ? games.map((game) => {
        const mechanics = game.mechanics?.length
          ? '<ol class="details">' + game.mechanics.map((m) =>
              '<li>' + esc(m.name) +
              (m.intendedAffect ? ' <span class="muted">[' + esc(m.intendedAffect) + ']</span>' : '') +
              (m.intends ? '<div class="muted">' + esc(m.intends) + '</div>' : '') +
              '</li>'
            ).join("") + '</ol>'
          : '<div class="muted" style="margin-top:8px;">mechanicなし</div>';
        const aesthetics = game.aesthetics?.length
          ? '<div class="muted" style="margin-top:8px;">aesthetic: ' + esc(game.aesthetics.map((a) => a.name).join(", ")) + '</div>'
          : "";
        return '<article class="item">' +
          '<div class="item-head">' +
            '<div class="item-title">' + esc(game.title) + '</div>' +
            '<div class="item-size muted">size ' + game.size + '</div>' +
          '</div>' +
          '<div class="muted">' + esc(game.genre || "genre unknown") +
            ' / mechanic ' + game.mechanicCount +
            ' / aesthetic ' + game.aestheticCount +
            ' / opinion ' + game.opinionCount + '</div>' +
          mechanics + aesthetics +
        '</article>';
      }).join("")
    : '<div class="muted">ゲーム学習データはまだありません</div>';
  renderGraph(games, "gameなし");
}

function renderOpinions(snap) {
  document.getElementById("totals").textContent =
    "話題と意見: topic " + snap.totals.topics +
    " / opinion " + snap.totals.opinions +
    " / utterance " + snap.totals.utterances;
  document.getElementById("graph-help").textContent =
    "円の大きさ = 意見数 x 3 + 発話数。";
  const topics = snap.topics || [];
  document.getElementById("items").innerHTML = topics.length
    ? topics.map((topic) => {
        const opinions = topic.opinions?.length
          ? '<ol class="details">' + topic.opinions.map((opinion) =>
              '<li>' + esc(opinion.statement) + ' <span class="muted">(' + esc(opinion.status || "open") + ')</span></li>'
            ).join("") + '</ol>'
          : '<div class="muted" style="margin-top:8px;">意見なし</div>';
        const affect = topic.expectedAffect || topic.observedAffect
          ? '<div class="muted">期待: ' + esc(topic.expectedAffect || "?") + ' / 観測: ' + esc(topic.observedAffect || "?") + '</div>'
          : "";
        return '<article class="item">' +
          '<div class="item-head">' +
            '<div class="item-title">' + esc(topic.title) + '</div>' +
            '<div class="item-size muted">size ' + topic.size + '</div>' +
          '</div>' +
          '<div class="muted">意見 ' + topic.opinionCount + ' / 発話 ' + topic.utteranceCount + '</div>' +
          (topic.description ? '<div class="muted">' + esc(topic.description) + '</div>' : '') +
          affect + opinions +
        '</article>';
      }).join("")
    : '<div class="muted">話題はまだありません</div>';
  renderGraph(topics, "話題なし");
}

function renderGraph(items, emptyLabel) {
  const svg = document.getElementById("graph");
  const graphItems = items.slice(0, 16);
  if (!graphItems.length) {
    svg.innerHTML = '<text x="280" y="220" text-anchor="middle">' + esc(emptyLabel) + '</text>';
    return;
  }
  const maxSize = Math.max(1, ...graphItems.map((item) => item.size || 1));
  const cols = 4;
  const cellW = 560 / cols;
  const cellH = 104;
  svg.innerHTML = graphItems.map((item, index) => {
    const col = index % cols;
    const row = Math.floor(index / cols);
    const cx = col * cellW + cellW / 2;
    const cy = row * cellH + 48;
    const radius = 14 + Math.sqrt((item.size || 1) / maxSize) * 34;
    const fill = item.status === "negative" || item.status === "rejected" ? "#dc2626" :
      item.status === "ambivalent" ? "#ca8a04" :
      item.status === "resolved" || item.status === "closed" ? "#6b7280" : "#2563eb";
    return '<g>' +
      '<circle cx="' + cx + '" cy="' + cy + '" r="' + radius.toFixed(1) + '" fill="' + fill + '" fill-opacity="0.24" stroke="' + fill + '" stroke-width="2"></circle>' +
      '<text x="' + cx + '" y="' + cy + '" text-anchor="middle" font-weight="700">' + esc(item.size ?? 0) + '</text>' +
      '<text x="' + cx + '" y="' + (cy + radius + 16).toFixed(1) + '" text-anchor="middle">' + esc(label(item.title || item.key, 16)) + '</text>' +
    '</g>';
  }).join("");
}

document.querySelectorAll("button[data-layer]").forEach((button) => {
  button.addEventListener("click", () => setActiveLayer(button.dataset.layer));
});
setActiveLayer(currentLayer);
</script>
</body>
</html>`;
