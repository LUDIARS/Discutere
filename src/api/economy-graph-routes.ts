/**
 * Ludus economy graph — REST API + loopback HTML graph viewer.
 *
 * Routes:
 *   POST /api/ludus/analyze-economy        trigger LLM analysis (async, fire-and-forget)
 *   GET  /api/ludus/economy-graph/:slug    graph JSON
 *   GET  /api/ludus/economy-graphs         list all cached graphs for workspace
 *   GET  /ludus/economy-graph/:slug        HTML graph page (vis-network CDN)
 */

import { Hono } from "hono";
import { economyGraphRepo } from "../db/repository.js";
import { analyzeEconomy, toSlug } from "../ludus/economy-analyzer.js";
import type { EconomyGraph } from "../ludus/economy-analyzer.js";

export interface EconomyGraphRoutesDeps {
  workspaceId: string;
  apiKey: string;
}

function buildGraphHtml(slug: string, apiBase: string): string {
  return `<!DOCTYPE html>
<html lang="ja">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<meta name="color-scheme" content="light dark" />
<title>Economy Graph</title>
<script src="https://cdnjs.cloudflare.com/ajax/libs/vis-network/9.1.9/dist/vis-network.min.js" crossorigin="anonymous"></script>
<style>
  :root { color-scheme: light; --bg:#f8fafc; --fg:#111827; --muted:#64748b; --muted-2:#94a3b8; --surface:#fff; --surface-soft:#f1f5f9; --graph-bg:#fff; --border:#d0d7de; --button-bg:#e2e8f0; --button-hover:#cbd5e1; --button-fg:#334155; --error:#dc2626; }
  @media (prefers-color-scheme: dark) { :root { color-scheme: dark; --bg:#0f1117; --fg:#e2e8f0; --muted:#a0aec0; --muted-2:#718096; --surface:#1a202c; --surface-soft:#2d3748; --graph-bg:#0a0e18; --border:#2d3748; --button-bg:#2d3748; --button-hover:#3d4a5c; --button-fg:#a0aec0; --error:#fc8181; } }
  *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: system-ui, sans-serif; background: var(--bg); color: var(--fg); min-height: 100vh; }
  #header { padding: 16px 20px; border-bottom: 1px solid var(--border); display: flex; align-items: center; gap: 12px; }
  #header h1 { font-size: 1.1rem; font-weight: 600; }
  #header .badge { font-size: 0.75rem; background: var(--surface-soft); padding: 2px 8px; border-radius: 9999px; color: var(--muted); }
  #graph-wrap { position: relative; height: 520px; margin: 16px 20px; border: 1px solid var(--border); border-radius: 8px; overflow: hidden; background: var(--graph-bg); }
  #graph { width: 100%; height: 100%; }
  #loading { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; background: var(--graph-bg); font-size: 0.9rem; color: var(--muted); }
  #legend { display: flex; flex-wrap: wrap; gap: 8px; padding: 0 20px 12px; }
  .legend-item { display: flex; align-items: center; gap: 5px; font-size: 0.75rem; color: var(--muted); }
  .legend-dot { width: 10px; height: 10px; border-radius: 50%; }
  #info { padding: 0 20px 20px; display: grid; gap: 16px; }
  .section-title { font-size: 0.75rem; text-transform: uppercase; letter-spacing: 0.08em; color: var(--muted-2); margin-bottom: 6px; }
  #summary-text { font-size: 0.9rem; line-height: 1.6; color: var(--fg); background: var(--surface); padding: 12px 16px; border-radius: 6px; }
  #loops-list { list-style: none; display: grid; gap: 6px; }
  #loops-list li { font-size: 0.85rem; color: var(--muted); background: var(--surface); padding: 8px 12px; border-radius: 6px; border-left: 3px solid #38a169; }
  #node-detail { background: var(--surface); padding: 12px 16px; border-radius: 6px; font-size: 0.85rem; color: var(--muted); min-height: 50px; }
  .meta-row { font-size: 0.75rem; color: var(--muted-2); margin-top: 4px; }
  #reanalyze { display: inline-block; margin: 0 20px 16px; padding: 6px 14px; background: var(--button-bg); border: 1px solid var(--border); border-radius: 6px; font-size: 0.8rem; color: var(--button-fg); cursor: pointer; }
  #reanalyze:hover { background: var(--button-hover); }
  #error-msg { color: var(--error); font-size: 0.9rem; padding: 20px; }
</style>
</head>
<body>
<div id="header">
  <h1 id="title">Economy Graph</h1>
  <span class="badge" id="node-count">-</span>
  <span class="badge" id="edge-count">-</span>
  <span class="badge" id="analyzed-at">-</span>
</div>

<div id="legend">
  <div class="legend-item"><div class="legend-dot" style="background:#f9a825"></div>Engine</div>
  <div class="legend-item"><div class="legend-dot" style="background:#c62828"></div>Attrition</div>
  <div class="legend-item"><div class="legend-dot" style="background:#1565c0"></div>Win Condition</div>
  <div class="legend-item"><div class="legend-dot" style="background:#00695c"></div>Resource</div>
  <div class="legend-item"><div class="legend-dot" style="background:#7b1fa2"></div>Experience</div>
</div>

<div id="graph-wrap">
  <div id="graph"></div>
  <div id="loading">グラフを読み込み中...</div>
</div>

<div id="info">
  <div>
    <div class="section-title">クリックしたノードの詳細</div>
    <div id="node-detail">ノードをクリックすると説明が表示されます</div>
  </div>
  <div>
    <div class="section-title">システム概要</div>
    <div id="summary-text">分析中...</div>
  </div>
  <div>
    <div class="section-title">フィードバックループ</div>
    <ul id="loops-list"></ul>
  </div>
</div>

<button id="reanalyze" onclick="reanalyze()">再分析する</button>

<script>
const SLUG = ${JSON.stringify(slug)};
const API_BASE = ${JSON.stringify(apiBase)};

const NODE_COLORS = {
  engine:        { background: '#f9a825', border: '#f57f17', highlight: { background: '#ffca28', border: '#f9a825' } },
  attrition:     { background: '#c62828', border: '#b71c1c', highlight: { background: '#ef5350', border: '#c62828' } },
  win_condition: { background: '#1565c0', border: '#0d47a1', highlight: { background: '#1e88e5', border: '#1565c0' } },
  resource:      { background: '#00695c', border: '#004d40', highlight: { background: '#00897b', border: '#00695c' } },
  experience:    { background: '#7b1fa2', border: '#4a148c', highlight: { background: '#9c27b0', border: '#7b1fa2' } },
};

const EDGE_COLORS = {
  produces:   '#43a047',
  consumes:   '#e53935',
  enables:    '#1e88e5',
  requires:   '#546e7a',
  drives:     '#8e24aa',
  suppresses: '#e91e63',
  leads_to:   '#ef6c00',
  reinforces: '#00897b',
  feedback:   '#c62828',
};

function buildNetwork(graph) {
  const nodeMap = {};
  graph.nodes.forEach(n => { nodeMap[n.id] = n; });

  const nodes = new vis.DataSet(graph.nodes.map(n => ({
    id: n.id,
    label: n.label,
    title: n.description,
    color: NODE_COLORS[n.type] ?? NODE_COLORS.engine,
    font: { color: '#fff', size: 13 },
    shape: n.type === 'win_condition' ? 'diamond' : n.type === 'experience' ? 'ellipse' : 'box',
    margin: 8,
  })));

  const edges = new vis.DataSet(graph.edges.map(e => ({
    id: e.id,
    from: e.source,
    to: e.target,
    label: e.label,
    title: e.type,
    color: { color: EDGE_COLORS[e.type] ?? '#718096', highlight: '#fff' },
    font: { color: '#a0aec0', size: 11, background: '#0a0e18' },
    arrows: { to: { enabled: true, scaleFactor: 0.7 } },
    dashes: ['enables','suppresses','reinforces'].includes(e.type),
    width: e.type === 'feedback' ? 2.5 : 1.5,
  })));

  const container = document.getElementById('graph');
  const network = new vis.Network(container, { nodes, edges }, {
    physics: {
      enabled: true,
      barnesHut: { gravitationalConstant: -3000, centralGravity: 0.3, springLength: 160 },
    },
    interaction: { tooltipDelay: 100 },
    layout: { improvedLayout: true },
  });

  network.on('click', params => {
    if (!params.nodes.length) { document.getElementById('node-detail').textContent = 'ノードをクリックすると説明が表示されます'; return; }
    const n = nodeMap[params.nodes[0]];
    if (n) {
      document.getElementById('node-detail').innerHTML =
        '<strong>' + escHtml(n.label) + '</strong> <span style="color:#718096;font-size:0.75rem">[' + n.type + ']</span><br>' +
        '<span style="color:#a0aec0">' + escHtml(n.description) + '</span>';
    }
  });

  document.getElementById('loading').style.display = 'none';
}

function escHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

async function loadGraph() {
  try {
    const res = await fetch(API_BASE + '/api/ludus/economy-graph/' + SLUG);
    if (!res.ok) {
      if (res.status === 404) {
        document.getElementById('loading').textContent = '⏳ 分析中です。しばらくしてからリロードしてください...';
        return;
      }
      throw new Error('HTTP ' + res.status);
    }
    const graph = await res.json();

    document.getElementById('title').textContent = 'Economy Graph: ' + graph.gameTitle;
    document.title = 'Economy Graph: ' + graph.gameTitle;
    document.getElementById('node-count').textContent = graph.nodes.length + ' nodes';
    document.getElementById('edge-count').textContent = graph.edges.length + ' edges';
    const at = graph.analyzedAt ? new Date(graph.analyzedAt).toLocaleString('ja-JP') : '';
    document.getElementById('analyzed-at').textContent = at;
    document.getElementById('summary-text').textContent = graph.summary || '(概要なし)';

    const loopsList = document.getElementById('loops-list');
    loopsList.innerHTML = '';
    (graph.economyLoops || []).forEach(l => {
      const li = document.createElement('li');
      li.textContent = l;
      loopsList.appendChild(li);
    });

    buildNetwork(graph);
  } catch (err) {
    document.getElementById('loading').style.display = 'none';
    document.getElementById('graph-wrap').innerHTML = '<div id="error-msg">⚠️ グラフの読み込みに失敗しました: ' + escHtml(err.message) + '</div>';
  }
}

async function reanalyze() {
  const btn = document.getElementById('reanalyze');
  btn.textContent = '分析中...';
  btn.disabled = true;
  try {
    const res = await fetch(API_BASE + '/api/ludus/analyze-economy', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ slug: SLUG, force: true }),
    });
    const data = await res.json();
    if (data.ok) { window.location.reload(); }
    else { alert('再分析エラー: ' + (data.error ?? 'unknown')); }
  } catch (err) {
    alert('再分析リクエスト失敗: ' + err.message);
  } finally {
    btn.textContent = '再分析する';
    btn.disabled = false;
  }
}

loadGraph();
</script>
</body>
</html>`;
}

export function createEconomyGraphRoutes(deps: EconomyGraphRoutesDeps) {
  const app = new Hono();

  app.post("/api/ludus/analyze-economy", async (c) => {
    const body = (await c.req.json().catch(() => ({}))) as { gameTitle?: string; slug?: string; force?: boolean };
    let gameTitle = body.gameTitle;

    if (!gameTitle && body.slug) {
      const cached = await economyGraphRepo.listByWorkspace(deps.workspaceId);
      const found = cached.find((g) => toSlug(g.gameTitle) === body.slug);
      gameTitle = found?.gameTitle;
    }

    if (!gameTitle) {
      return c.json({ ok: false, error: "gameTitle or slug required" }, 400);
    }

    void analyzeEconomy(deps.workspaceId, gameTitle, deps.apiKey, { force: body.force }).catch(
      (err) => console.error(`[economy-graph] analyze error: ${(err as Error).message}`),
    );

    return c.json({ ok: true, slug: toSlug(gameTitle), message: "analysis started" });
  });

  app.get("/api/ludus/economy-graphs", async (c) => {
    const graphs = await economyGraphRepo.listByWorkspace(deps.workspaceId);
    return c.json(
      graphs.map((g) => ({
        slug: toSlug(g.gameTitle),
        gameTitle: g.gameTitle,
        mechanicCount: g.mechanicCount,
        updatedAt: g.updatedAt,
      })),
    );
  });

  app.get("/api/ludus/economy-graph/:slug", async (c) => {
    const slug = c.req.param("slug");
    const graphs = await economyGraphRepo.listByWorkspace(deps.workspaceId);
    const match = graphs.find((g) => toSlug(g.gameTitle) === slug);
    if (!match) return c.json({ error: "not found" }, 404);
    return c.json(JSON.parse(match.graphJson) as EconomyGraph);
  });

  app.get("/ludus/economy-graph/:slug", (c) => {
    const slug = c.req.param("slug");
    const proto = c.req.header("x-forwarded-proto") ?? "http";
    const host = c.req.header("host") ?? "localhost";
    const apiBase = `${proto}://${host}`;
    return c.html(buildGraphHtml(slug, apiBase));
  });

  return app;
}
