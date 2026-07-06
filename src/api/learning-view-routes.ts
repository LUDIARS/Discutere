import { Hono } from "hono";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import {
  buildLearningLayerSnapshot,
  normalizeLearningLayer,
} from "../visualize/learning-layers.js";
import { listConclusions, getConclusionDetail, type ConclusionDetail } from "../visualize/conclusions.js";
import {
  listFlowConclusions,
  getFlowConclusionDetail,
  isFlowConclusionId,
  flowSessionIdFromGapId,
} from "../visualize/flow-conclusions.js";
import { renderConclusionMarkdown, safeSlug } from "../visualize/conclusion-markdown.js";
import {
  conclusionCacheCount,
  ensureConclusionCacheFresh,
  listCachedConclusions,
} from "../visualize/conclusion-cache.js";
import { getFlowDb } from "../flow/db/connection.js";
import { openAttributionStore } from "../crawler/sources/attribution-store.js";
import { openLearningCacheReader, learningCacheExists } from "../visualize/learning-cache.js";

export const learningViewRoutes = new Hono();

learningViewRoutes.get("/learning", (c) => c.html(HTML));

// Gap率 (運営想定 vs 観測) は事前ビルドの cache から配信 (build:learning-cache で焼く)。
learningViewRoutes.get("/learning/gap", (c) => {
  if (!learningCacheExists()) return c.json({ games: [] });
  const reader = openLearningCacheReader();
  try {
    return c.json(reader.gapSummary() as object);
  } finally {
    reader.close();
  }
});

learningViewRoutes.get("/learning/gap/detail", (c) => {
  if (!learningCacheExists()) return c.json({ error: "no cache" }, 404);
  const reader = openLearningCacheReader();
  try {
    const detail = reader.gap(c.req.query("slug") ?? "");
    return detail ? c.json(detail as object) : c.json({ error: "not found" }, 404);
  } finally {
    reader.close();
  }
});

// データソース構成 (どの取得元が何件か) は cache から配信 (build:learning-cache で焼く)。
learningViewRoutes.get("/learning/sources", (c) => {
  if (!learningCacheExists()) return c.json({ totals: { sources: 0 }, sources: [], byGame: [] });
  const reader = openLearningCacheReader();
  try {
    return c.json(reader.dataSources() as object);
  } finally {
    reader.close();
  }
});

// 収束した議論の結論一覧 (#66 — Learning View 統合)。
// 既定は SQLite キャッシュ (conclusion_cache) から読む = KG (481MB) 非タッチで高速
// (議論収束ごとに write-through 更新 + build:conclusion-cache で材料件数算出)。
// キャッシュ未構築 (0 件) のときだけ従来の live マージ (KG 直読) にフォールバックする。
learningViewRoutes.get("/learning/conclusions", (c) => {
  const config = getConfig();
  const limit = Number(c.req.query("limit") ?? 100);
  const flowDb = getFlowDb();

  // 新フロー結論をキャッシュへ追いつかせる (KG 非依存・追いついていれば COUNT 2 回で即 return)。
  ensureConclusionCacheFresh(flowDb);

  if (conclusionCacheCount(flowDb) > 0) {
    return c.json({ conclusions: listCachedConclusions(flowDb, limit), cached: true });
  }

  // フォールバック: キャッシュ未構築。live で KG + flow をマージ (重い)。
  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    const gapConclusions = listConclusions(core, config.workspace, limit);
    const flowConclusions = listFlowConclusions(flowDb, limit);
    const merged = [...gapConclusions, ...flowConclusions]
      .sort((a, b) => b.updatedAt - a.updatedAt)
      .slice(0, limit);
    return c.json({ conclusions: merged, cached: false });
  } finally {
    core.close();
  }
});

// 1 件の結論の裏の論述データ (議論ログ / 止揚 / 高評価意見)。
// gap="flow:<sessionId>" は新フロー、それ以外は旧フロー (design_gap) としてルートする。
learningViewRoutes.get("/learning/conclusion", (c) => {
  const config = getConfig();
  const gapId = c.req.query("gap") ?? "";
  if (isFlowConclusionId(gapId)) {
    const detail = getFlowConclusionDetail(getFlowDb(), flowSessionIdFromGapId(gapId));
    if (!detail) return c.json({ error: "not found" }, 404);
    return c.json(detail);
  }
  const core = createCore(resolveActiveKgPath(getConfig()));
  // 出所メタ (source/sourceUrl) を発話に付ける (§6 露出制御。 個人マスクは serializer 側)。
  const attribution = openAttributionStore();
  try {
    const detail = getConclusionDetail(core, config.workspace, gapId, attribution);
    if (!detail) return c.json({ error: "not found" }, 404);
    return c.json(detail);
  } finally {
    attribution.close();
    core.close();
  }
});

// 1 件の議論を単体 md ファイルとしてダウンロードする (議論 md エクスポート)。
learningViewRoutes.get("/learning/conclusion/export", (c) => {
  const config = getConfig();
  const gapId = c.req.query("gap") ?? "";
  let detail: ConclusionDetail | null;
  let core: ReturnType<typeof createCore> | null = null;
  let attribution: ReturnType<typeof openAttributionStore> | null = null;
  try {
    if (isFlowConclusionId(gapId)) {
      detail = getFlowConclusionDetail(getFlowDb(), flowSessionIdFromGapId(gapId));
    } else {
      core = createCore(resolveActiveKgPath(getConfig()));
      attribution = openAttributionStore();
      detail = getConclusionDetail(core, config.workspace, gapId, attribution);
    }
    if (!detail) return c.json({ error: "not found" }, 404);
    const md = renderConclusionMarkdown(detail);
    const filename = `${safeSlug(detail.title, detail.sessionId || "discussion")}.md`;
    c.header("Content-Type", "text/markdown; charset=utf-8");
    c.header(
      "Content-Disposition",
      `attachment; filename="discussion.md"; filename*=UTF-8''${encodeURIComponent(filename)}`
    );
    return c.body(md);
  } finally {
    attribution?.close();
    core?.close();
  }
});

learningViewRoutes.get("/learning/data", (c) => {
  const config = getConfig();
  const layer = normalizeLearningLayer(c.req.query("layer"));
  const limit = Number(c.req.query("limit") ?? 60);
  const detailLimit = Number(c.req.query("detailLimit") ?? c.req.query("opinionsPerTopic") ?? 8);
  const core = createCore(resolveActiveKgPath(getConfig()));
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

export const HTML = `<!doctype html>
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
  input, textarea, select { font: inherit; color: inherit; background: transparent; border: 1px solid var(--line); border-radius: 6px; padding: 7px 9px; }
  textarea { width: 100%; min-height: 90px; resize: vertical; }
  .train { border: 1px solid var(--line); border-radius: 8px; padding: 12px; margin-bottom: 14px; }
  .train h2 { margin: 0 0 10px; font-size: 15px; }
  .train-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }
  .train-grid label, .train-full { display: grid; gap: 4px; }
  .train-actions { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; margin-top: 10px; }
  .train-status { white-space: pre-wrap; }
  a.md-btn { display: inline-block; border: 1px solid var(--line); border-radius: 6px; padding: 6px 11px; text-decoration: none; color: inherit; font-size: 13px; }
  a.md-btn:hover { border-color: var(--accent); }
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
  @media (max-width: 860px) { main { grid-template-columns: 1fr; } .graph-wrap { position: static; } svg { height: 320px; } .train-grid { grid-template-columns: 1fr; } }
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
    <button data-layer="gap">Gap率</button>
    <button data-layer="sources">データソース</button>
  </div>
  <div class="muted" id="totals">loading...</div>
</header>
<main>
  <section>
    <div class="train">
      <h2>学習</h2>
      <form id="trainForm">
        <div class="train-grid">
          <label>ゲームタイトル <input id="trainGameTitle" type="text" required placeholder="Anatomia" /></label>
          <label>学習テーマ <input id="trainTheme" type="text" required placeholder="ゲーム内容 / ユーザの反応" /></label>
          <label>ユーザの声検索クエリ <input id="trainQuery" type="text" placeholder="game title review gameplay" /></label>
          <label>Steam appId <input id="trainSteamAppId" type="number" min="1" placeholder="1145360" /></label>
          <label>Reddit subreddit <input id="trainSubreddit" type="text" placeholder="games" /></label>
          <label>仕様書 URL / local path <input id="trainSpecUrl" type="text" placeholder="https://... または docs/spec.md" /></label>
          <label>GitHub repo URL <input id="trainGithubRepoUrl" type="text" placeholder="https://github.com/owner/repo" /></label>
          <label>GitHub file path / ref <span><input id="trainGithubPath" type="text" placeholder="docs/spec.md" style="width:62%" /> <input id="trainGithubRef" type="text" placeholder="main" style="width:28%" /></span></label>
          <label>Anatomia project <input id="trainAnatomiaProject" type="text" placeholder="registered project name" /></label>
          <label>Anatomia repo path <input id="trainAnatomiaRepo" type="text" placeholder="E:/Document/Ars/AnatomiaProject" /></label>
        </div>
        <label class="train-full" style="margin-top:10px;">システム / メカニクス説明
          <textarea id="trainMechanics" placeholder="基本ループ、主要メカニクス、報酬、制約、想定される体験"></textarea>
        </label>
        <div class="train-actions">
          <input id="trainSpecFile" type="file" accept=".doc,.docx,.txt,.text,.xlsx,.xslx,.pptx,.html,.htm,.pdf,.md,.markdown,.yaml,.yml,.json,text/*" />
          <button id="trainGithubCheck" type="button">GitHub read check</button>
          <button id="trainSubmit" type="submit">学習を実行</button>
        </div>
        <pre id="trainGithubAuth" class="muted train-status"></pre>
        <div id="trainStatus" class="muted train-status"></div>
      </form>
    </div>
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

const TRAIN_SPEC_EXTS = new Set([".doc", ".docx", ".txt", ".text", ".xlsx", ".xslx", ".pptx", ".html", ".htm", ".pdf", ".md", ".markdown", ".yaml", ".yml", ".json"]);
function byId(id) { return document.getElementById(id); }
function trainExt(name) {
  const i = String(name).lastIndexOf(".");
  return i >= 0 ? String(name).slice(i).toLowerCase() : "";
}
function appendTrainMechanics(text) {
  const el = byId("trainMechanics");
  const cur = el.value.trim();
  el.value = cur ? cur + "\\n\\n" + text : text;
}
async function loadTrainGithubAuth() {
  const el = byId("trainGithubAuth");
  const res = await fetch("/api/spec/github/auth").then(r => r.json()).catch(() => null);
  el.textContent = res && res.ok && res.auth ? (res.auth.output || "gh auth status unavailable") : "gh auth status unavailable";
}
async function checkTrainGithub() {
  const status = byId("trainStatus");
  status.textContent = "Checking GitHub read...";
  const body = {
    repoUrl: byId("trainGithubRepoUrl").value.trim(),
    path: byId("trainGithubPath").value.trim(),
    ref: byId("trainGithubRef").value.trim(),
  };
  const res = await fetch("/api/spec/github/check", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  }).then(r => r.json()).catch(() => null);
  if (!res) { status.textContent = "GitHub check failed"; return; }
  if (!res.ok) { status.textContent = res.error || "GitHub check failed"; return; }
  if (res.auth && res.auth.output) byId("trainGithubAuth").textContent = res.auth.output;
  if (res.text) appendTrainMechanics(res.text);
  status.textContent = res.text ? "GitHub read OK; text appended." : "GitHub repo read OK.";
}
async function uploadTrainSpec(file) {
  const status = byId("trainStatus");
  if (!TRAIN_SPEC_EXTS.has(trainExt(file.name))) {
    status.textContent = "Unsupported file format: " + file.name;
    return;
  }
  const fd = new FormData();
  fd.append("file", file);
  status.textContent = "Extracting spec file...";
  const res = await fetch("/api/spec/extract", { method: "POST", body: fd }).then(r => r.json()).catch(() => null);
  if (!res || !res.ok) { status.textContent = "Spec extraction failed: " + (res && res.error ? res.error : "unknown"); return; }
  appendTrainMechanics(res.text);
  status.textContent = "Spec text appended.";
}
async function submitTrain(e) {
  e.preventDefault();
  const status = byId("trainStatus");
  const gameTitle = byId("trainGameTitle").value.trim();
  const discussionTheme = byId("trainTheme").value.trim();
  if (!gameTitle || !discussionTheme) { status.textContent = "Game title and theme are required."; return; }
  const learningAppId = byId("trainSteamAppId").value.trim() ? Number(byId("trainSteamAppId").value) : undefined;
  const payload = {
    flow: "learning",
    gameTitle,
    discussionTheme,
    theme: [gameTitle, discussionTheme].join(" / "),
    mechanicsContext: byId("trainMechanics").value.trim() || undefined,
    specText: byId("trainMechanics").value.trim() || undefined,
    specUrl: byId("trainSpecUrl").value.trim() || undefined,
    githubRepoUrl: byId("trainGithubRepoUrl").value.trim() || undefined,
    githubPath: byId("trainGithubPath").value.trim() || undefined,
    githubRef: byId("trainGithubRef").value.trim() || undefined,
    anatomiaProject: byId("trainAnatomiaProject").value.trim() || undefined,
    anatomiaRepo: byId("trainAnatomiaRepo").value.trim() || undefined,
    learningSource: "balanced",
    learningBalanced: true,
    learningQuery: byId("trainQuery").value.trim() || undefined,
    learningAppId,
    learningSubreddit: byId("trainSubreddit").value.trim() || undefined,
  };
  byId("trainSubmit").disabled = true;
  status.textContent = "Learning...";
  const res = await fetch("/api/flow/start", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(payload),
  }).then(r => r.json()).catch(() => null);
  byId("trainSubmit").disabled = false;
  if (!res || !res.ok) { status.textContent = "Learning failed: " + (res && res.error ? res.error : "unknown"); return; }
  const r = res.result || {};
  status.textContent =
    "Learning complete\\n" +
    "game: " + (r.gameSlug || res.sessionId || "") + "\\n" +
    "mechanics: " + (r.mechanicsRecorded || 0) + "\\n" +
    "user voices: " + ((r.opinionsRecorded || 0) + (r.crawledImported || 0)) + "\\n" +
    "synthetic voices: " + (res.syntheticOpinions || 0) + "\\n" +
    "by source: " + JSON.stringify(r.crawledBySource || {});
  setActiveLayer("games");
}

function label(value, max) {
  const text = String(value ?? "");
  return text.length <= max ? text : text.slice(0, max - 1) + "...";
}

// 出所バッジ (§6: ソース種別 + 元 URL を開示。 個人は仮名のまま)。 出所無しは空。
function sourceBadge(o) {
  if (!o || !o.source) return "";
  const label = ' <span class="muted">[' + esc(o.source) + ' ↗]</span>';
  return o.sourceUrl
    ? ' <a href="' + esc(o.sourceUrl) + '" target="_blank" rel="noopener" class="muted">[' + esc(o.source) + ' ↗]</a>'
    : label;
}

// ディスカッションペーパー (議題ブリーフ)。 新フロー結論のみ持つ (旧議論は paper=null → 空)。
function paperBlock(paper) {
  if (!paper) return "";
  const parts = [];
  if ((paper.tags || []).length) {
    parts.push('<div class="muted" style="margin-top:4px;">観点タグ: ' + paper.tags.map(esc).join(" / ") + '</div>');
  }
  if (paper.supplement) {
    parts.push('<div style="margin-top:4px;"><span class="muted">観点補足:</span> ' + esc(paper.supplement) + '</div>');
  }
  if ((paper.mechanics || []).length) {
    parts.push('<div style="margin-top:4px;"><span class="muted">ゲームのメカニクス</span><ol class="details">' +
      paper.mechanics.map((m) =>
        '<li><b>' + esc(m.name) + '</b>' + (m.description ? ': ' + esc(m.description) : '') +
        (m.intendedAffect ? ' <span class="muted">→ 期待感情: ' + esc(m.intendedAffect) + '</span>' : '') + '</li>'
      ).join("") + '</ol></div>');
  }
  if (!parts.length) return "";
  return '<div style="margin-top:8px;"><b>ディスカッションペーパー</b>' + parts.join("") + '</div>';
}

async function load(layer) {
  if (layer === "gap") {
    const res = await fetch("/learning/gap");
    if (!res.ok) throw new Error("gap http " + res.status);
    return res.json();
  }
  if (layer === "conclusions") {
    const res = await fetch("/learning/conclusions?limit=200");
    if (!res.ok) throw new Error("conclusions http " + res.status);
    return res.json();
  }
  if (layer === "sources") {
    const res = await fetch("/learning/sources");
    if (!res.ok) throw new Error("sources http " + res.status);
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
  if (currentLayer === "gap") return renderGap(snap);
  if (currentLayer === "sources") return renderSources(snap);
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
            '<div class="item-title">' + esc(c.title) +
              ' <span class="muted">[' + (c.kind === "flow" ? "新フロー" : "旧") + ']</span></div>' +
            '<div class="item-size muted">発話 ' + c.utteranceCount + ' / 止揚 ' + c.aufhebungCount + '</div>' +
          '</div>' +
          '<div style="margin-top:6px;">' + esc(c.conclusion || "(まとめ未生成)") + '</div>' +
          '<button class="detail-btn" data-gap="' + esc(c.gapId) + '" style="margin-top:8px;">論述データを見る</button> ' +
          '<a class="md-btn" href="/learning/conclusion/export?gap=' + encodeURIComponent(c.gapId) + '" download>md エクスポート</a>' +
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
        d.topOpinions.map((o) => '<li>+' + o.score + ' ' + esc(o.speaker) + ': ' + esc(o.content) + sourceBadge(o) + '</li>').join("") + '</ol></div>' : "";
    const log = '<div style="margin-top:8px;"><b>議論ログ (' + (d.transcript || []).length + '発話)</b><ol class="details">' +
      (d.transcript || []).map((u) => '<li><span class="muted">[' + esc(u.speaker) + ']</span> ' + esc(u.content) + sourceBadge(u) + '</li>').join("") + '</ol></div>';
    slot.innerHTML = paperBlock(d.paper) + auf + top + log;
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

function pctg(value) { return (Math.round((value || 0) * 1000) / 10) + "%"; }

function kindLabel(kind) {
  return kind === "import" ? "取込" : kind === "derived" ? "派生" : "内部生成";
}

function renderSources(snap) {
  const sources = snap.sources || [];
  const totals = snap.totals || {};
  document.getElementById("totals").textContent =
    "データソース: " + (totals.sources || 0) + "種 (取込 " + (totals.importSources || 0) +
    " / 内部 " + (totals.internalSources || 0) + ")" +
    " / 総発話 " + (totals.utterances || 0) +
    " (うち取込 " + (totals.importUtterances || 0) + ")" +
    " / ゲーム " + (totals.games || 0);
  document.getElementById("graph-help").textContent = "円の大きさ = 発話数 / 青=取込・橙=派生・灰=内部生成。不要な取得元は CLI crawl.ts quarantine <source> [slug] で退避+隔離 (復元可)。";
  const byGame = snap.byGame || [];
  const gameTable = byGame.length
    ? '<article class="item"><div class="item-title">ゲーム別 取得元内訳</div>' +
      byGame.map((g) =>
        '<div style="margin-top:8px;"><b>' + esc(g.slug) + '</b> <span class="muted">発話 ' + g.utterances + ' / ' + g.sources.length + 'ソース</span>' +
        '<ol class="details">' + g.sources.map((s) =>
          '<li>' + esc(s.label) + ' <span class="muted">[' + kindLabel(s.kind) + '] ' + s.sessions + '件 / 発話 ' + s.utterances + '</span></li>'
        ).join("") + '</ol></div>'
      ).join("") + '</article>'
    : "";
  document.getElementById("items").innerHTML = (sources.length
    ? sources.map((s) =>
        '<article class="item">' +
          '<div class="item-head">' +
            '<div class="item-title">' + esc(s.label) + ' <span class="muted">[' + kindLabel(s.kind) + ']</span></div>' +
            '<div class="item-size muted">発話 ' + s.utterances + '</div>' +
          '</div>' +
          '<div class="muted">' + esc(s.source) + ' / ' + esc(s.origin) + '</div>' +
          '<div class="muted">取得単位(session) ' + s.sessions + ' / 対象ゲーム ' + s.gameCount +
            (s.games && s.games.length ? ': ' + esc(s.games.join(", ")) : '') + '</div>' +
        '</article>').join("")
    : '<div class="muted">データソースがありません</div>') + gameTable;
  renderGraph(sources.map((s) => ({
    title: s.label,
    size: s.utterances,
    status: s.kind === "import" ? "open" : s.kind === "derived" ? "ambivalent" : "closed",
  })), "ソースなし");
}

function renderGap(snap) {
  const games = snap.games || [];
  document.getElementById("totals").textContent =
    "Gap率: 運営の想定感情 vs 観測 (" + games.length + "ゲーム / Gap率降順)";
  document.getElementById("graph-help").textContent = "円の大きさ = 総発話数 / 色 = Gap率(高=赤)。";
  document.getElementById("items").innerHTML = games.length
    ? games.map((g) =>
        '<article class="item">' +
          '<div class="item-head"><div class="item-title">' + esc(g.game) + '</div>' +
            '<div class="item-size muted">Gap ' + pctg(g.gapRate) + '</div></div>' +
          '<div class="muted">総発話 ' + g.total + ' / カバレッジ ' + pctg(g.coverage) +
            ' / 意図外負 ' + pctg(g.negativeRate) +
            (g.topGapMechanic ? ' / 最大Gap: ' + esc(g.topGapMechanic) : '') + '</div>' +
          '<button class="gap-btn" data-slug="' + esc(g.slug) + '" style="margin-top:8px;">多角内訳を見る</button>' +
          '<div class="gap-slot"></div>' +
        '</article>').join("")
    : '<div class="muted">Gapデータなし (data/games の md に運営想定を定義 → build:learning-cache)</div>';
  document.querySelectorAll(".gap-btn").forEach((b) => b.addEventListener("click", () => loadGapDetail(b)));
  renderGraph(games.map((g) => ({
    title: g.game,
    size: g.total,
    status: g.gapRate >= 0.5 ? "negative" : g.gapRate >= 0.3 ? "ambivalent" : "open",
  })), "Gapなし");
}

async function loadGapDetail(btn) {
  const slug = btn.dataset.slug;
  const slot = btn.parentElement.querySelector(".gap-slot");
  slot.innerHTML = '<div class="muted">loading...</div>';
  try {
    const res = await fetch("/learning/gap/detail?slug=" + encodeURIComponent(slug));
    if (!res.ok) throw new Error("detail http " + res.status);
    const d = await res.json();
    const mech = '<div style="margin-top:8px;"><b>mechanic別 Gap率</b><ol class="details">' +
      (d.byMechanic || []).filter((m) => m.attributed > 0).map((m) =>
        '<li>' + esc(m.name) + ' <span class="muted">想定' + esc(m.intendedValence) +
        ' / 帰属' + m.attributed + ' → gap ' + pctg(m.gapRate) + ' / neg ' + pctg(m.negativeRate) + '</span></li>'
      ).join("") + '</ol></div>';
    const asp = '<div style="margin-top:8px;"><b>感情次元別 負率</b><ol class="details">' +
      (d.byAspect || []).slice(0, 6).map((a) =>
        '<li>' + (a.intended ? "★" : "") + esc(a.aspect) +
        ' <span class="muted">言及' + a.mentions + ' / neg ' + pctg(a.negativeRate) + '</span></li>'
      ).join("") + '</ol></div>';
    const src = '<div style="margin-top:8px;"><b>ソース別 Gap率</b><ol class="details">' +
      (d.bySource || []).map((s) =>
        '<li>' + esc(s.key) + ' <span class="muted">n' + s.n + ' / gap ' + pctg(s.gapRate) + ' / neg ' + pctg(s.negativeRate) + '</span></li>'
      ).join("") + '</ol></div>';
    slot.innerHTML = mech + asp + src;
    btn.style.display = "none";
  } catch (err) {
    slot.innerHTML = '<div class="muted">' + esc(err.message) + '</div>';
  }
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
document.getElementById("trainForm").addEventListener("submit", submitTrain);
document.getElementById("trainGithubCheck").addEventListener("click", () => { void checkTrainGithub(); });
document.getElementById("trainSpecFile").addEventListener("change", (e) => {
  const file = e.target.files && e.target.files[0];
  if (file) void uploadTrainSpec(file);
});
void loadTrainGithubAuth();
setActiveLayer(currentLayer);
</script>
</body>
</html>`;
