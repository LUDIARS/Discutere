/**
 * 簡素な議論フロー WebUI (loopback 1 ページ、依存ゼロの静的 HTML)。
 * テーマ入力 + 議論タイプ (必須) + タグ選択 + 送信。進行はポーリングで表示する。
 */

export const FLOW_HTML = `<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>Discutere 議論フロー</title>
<style>
  body { font-family: system-ui, sans-serif; max-width: 760px; margin: 1.5rem auto; padding: 0 1rem; color: #1a1a1a; }
  h1 { font-size: 1.2rem; }
  fieldset { border: 1px solid #ddd; border-radius: 8px; margin-bottom: 1rem; }
  label { display: block; margin: 0.4rem 0; }
  textarea, select { width: 100%; padding: 0.5rem; border: 1px solid #ccc; border-radius: 6px; font: inherit; box-sizing: border-box; }
  .tags label { display: inline-block; margin-right: 1rem; }
  button { padding: 0.5rem 1.2rem; border: 0; border-radius: 6px; background: #2563eb; color: #fff; font: inherit; cursor: pointer; }
  button:disabled { background: #9ca3af; cursor: default; }
  #log { margin-top: 1rem; }
  .u { padding: 0.4rem 0.6rem; border-left: 3px solid #cbd5e1; margin: 0.3rem 0; }
  .u.user { border-color: #2563eb; }
  .u .who { font-weight: 600; font-size: 0.85rem; color: #475569; }
  .err { color: #b91c1c; }
  #conclusion { margin-top: 1rem; padding: 0.8rem; background: #f0fdf4; border: 1px solid #bbf7d0; border-radius: 8px; white-space: pre-wrap; }
  #say { display: none; margin-top: 1rem; }
  .muted { color: #64748b; font-size: 0.85rem; }
  #review { margin-top: 1rem; padding: 0.8rem; background: #fffbeb; border: 1px solid #fde68a; border-radius: 8px; }
  #review h2 { font-size: 1rem; margin: 0 0 0.5rem; }
  #review input[type=text] { padding: 0.4rem; border: 1px solid #ccc; border-radius: 6px; font: inherit; }
  #review .row { margin: 0.5rem 0; }
  /* Notion 風ブロックエディタ */
  .blk { background: #fff; border: 1px solid #e5e7eb; border-radius: 8px; padding: 0.5rem 0.6rem; margin: 0.5rem 0; }
  .blk.heading { background: #f8fafc; }
  .blk .blk-type { font-size: 0.7rem; color: #94a3b8; margin-bottom: 0.2rem; }
  .blk .blk-text { width: 100%; border: 1px solid #e5e7eb; border-radius: 6px; padding: 0.4rem; font: inherit; box-sizing: border-box; resize: vertical; }
  .blk .blk-actions { margin-top: 0.3rem; display: flex; gap: 0.3rem; flex-wrap: wrap; }
  .blk .blk-actions button { padding: 0.25rem 0.6rem; font-size: 0.8rem; background: #e2e8f0; color: #1e293b; }
  .blk .blk-actions button.primary { background: #2563eb; color: #fff; }
  .blk .blk-actions button.danger { background: #fee2e2; color: #b91c1c; }
  .blk .proposal { margin-top: 0.4rem; padding: 0.4rem 0.5rem; background: #f1f5f9; border-radius: 6px; }
  .blk .proposal .rationale { font-size: 0.8rem; color: #475569; margin-bottom: 0.3rem; }
  .blk .proposal .old { background: #fee2e2; text-decoration: line-through; padding: 0.2rem 0.4rem; border-radius: 4px; white-space: pre-wrap; display: block; margin-bottom: 0.2rem; }
  .blk .proposal .new { background: #dcfce7; padding: 0.2rem 0.4rem; border-radius: 4px; white-space: pre-wrap; display: block; }
  .rvbar { margin-top: 0.8rem; display: flex; gap: 0.3rem; flex-wrap: wrap; align-items: center; }
  .rvbar input[type=text] { flex: 1; min-width: 14rem; }
  #rvRevert { background: #e2e8f0; color: #1e293b; }
  #rvRevert:disabled { background: #f1f5f9; color: #cbd5e1; }
  .confirm { margin-top: 0.8rem; padding-top: 0.6rem; border-top: 1px solid #fde68a; display: flex; align-items: center; gap: 0.8rem; }
  #rvApprove { background: #16a34a; }
  #rvApprove:disabled { background: #9ca3af; }
</style>
</head>
<body>
  <h1>Discutere 議論フロー</h1>
  <form id="start">
    <fieldset>
      <legend>テーマ</legend>
      <textarea id="theme" rows="3" placeholder="議題を入力" required></textarea>
    </fieldset>
    <fieldset>
      <legend>議論タイプ (必須)</legend>
      <select id="flow" required>
        <option value="">— 選択してください —</option>
        <option value="discussion">議論</option>
        <option value="improvement">改善</option>
        <option value="learning">学習 (収集)</option>
        <option value="sparring">壁打ち</option>
      </select>
    </fieldset>
    <fieldset class="tags">
      <legend>タグ</legend>
      <label><input type="checkbox" name="tag" value="機密" /> 機密</label>
      <label><input type="checkbox" name="tag" value="内部" /> 内部</label>
      <label><input type="checkbox" name="tag" value="運用" /> 運用</label>
      <label><input type="checkbox" name="tag" value="開発" /> 開発</label>
    </fieldset>
    <fieldset class="tags">
      <legend>進行量 (議論/改善のみ・空欄で既定)</legend>
      <label>ラウンド数 <input id="rounds" type="number" min="1" max="10" placeholder="既定" style="width:5rem" /></label>
      <label>ターン数/ラウンド <input id="turnsPerRound" type="number" min="1" max="20" placeholder="既定" style="width:5rem" /></label>
    </fieldset>
    <fieldset class="tags">
      <legend>壁打ち相手 (壁打ちのみ・任意)</legend>
      <label>ペルソナ名/ID (カンマ区切り) <input id="opponent" type="text" placeholder="例: ローグ好き太郎,ソシャゲ花子" style="width:60%" /></label>
    </fieldset>
    <fieldset class="tags">
      <legend>学習データ自動取得 (議論/改善のみ・学習データ不足時だけ実行)</legend>
      <label>ソース
        <select id="learningSource" style="width:auto">
          <option value="">既定 (config)</option>
          <option value="niconico">ニコニコ (テーマ検索・キー不要)</option>
          <option value="youtube">YouTube (テーマ検索・APIキー要)</option>
          <option value="steam">Steam (appId 指定)</option>
          <option value="website">Web サイト (URL 指定)</option>
        </select>
      </label>
      <label>検索クエリ (niconico/youtube・空欄でテーマ) <input id="learningQuery" type="text" placeholder="既定: テーマ文字列" style="width:60%" /></label>
      <label>Steam appId (steam 選択時) <input id="learningAppId" type="number" min="1" placeholder="例: 1145360" style="width:10rem" /></label>
      <label>URL (website 選択時・カンマ/改行区切り) <input id="learningUrls" type="text" placeholder="https://example.com/article" style="width:60%" /></label>
    </fieldset>
    <fieldset class="tags">
      <legend>仕様書の解析学習 (学習のみ・任意)</legend>
      <label>仕様書テキスト (貼り付け → 遊びのメカニクスを LLM 抽出して記録)
        <textarea id="specText" rows="6" placeholder="ゲーム仕様書を貼り付け (空欄なら解析しない)"></textarea>
      </label>
      <label>ファイルから読み込む (md/txt/json 等のテキスト → 上の欄へ展開)
        <input id="specFile" type="file" accept=".md,.txt,.json,.markdown,.text,text/*" />
      </label>
      <label>URL / ローカルパス (取得して解析・貼付本文と結合)
        <input id="specUrl" type="text" placeholder="https://… または spec/feature/foo.md" style="width:80%" />
      </label>
    </fieldset>
    <button type="submit" id="go">開始</button>
  </form>

  <div id="say">
    <input id="sayText" type="text" placeholder="壁打ち発話 (「まとめて」「おわり」で操作)" style="width:75%" />
    <button id="sayBtn">送信</button>
  </div>

  <div id="review" style="display:none">
    <h2>📝 ディスカッションペーパー編集 (確定すると議論開始できます)</h2>
    <div id="reviewInfo" class="muted">準備中…</div>
    <div class="row tags">観点タグ:
      <label><input type="checkbox" value="機密" /> 機密</label>
      <label><input type="checkbox" value="内部" /> 内部</label>
      <label><input type="checkbox" value="運用" /> 運用</label>
      <label><input type="checkbox" value="開発" /> 開発</label>
    </div>
    <div id="blocks"></div>
    <div class="rvbar">
      <input id="rvEdit" type="text" placeholder="全体を自然文で調整 (例: メカニクスにガチャを追加)" />
      <button id="rvEditBtn" type="button">全体調整</button>
      <button id="rvRevert" type="button" disabled>↶ 戻す</button>
    </div>
    <div id="rvMsg" class="muted"></div>
    <div class="confirm">
      <label><input type="checkbox" id="rvConfirm" /> ペーパーを確定する</label>
      <button id="rvApprove" type="button" disabled>議論開始</button>
    </div>
  </div>

  <div id="log"></div>
  <div id="conclusion" style="display:none"></div>

<script>
const $ = (id) => document.getElementById(id);
let sessionId = null, kind = null, since = 0, timer = null;

// 仕様書ファイル選択: テキストとして読み、上の specText 欄へ展開する (サーバ送信は specText で共通)。
$("specFile").addEventListener("change", (e) => {
  const file = e.target.files && e.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = () => {
    const text = String(reader.result || "");
    const cur = $("specText").value.trim();
    $("specText").value = cur ? (cur + "\n\n" + text) : text;
  };
  reader.onerror = () => alert("ファイルの読み込みに失敗しました");
  reader.readAsText(file);
});

$("start").addEventListener("submit", async (e) => {
  e.preventDefault();
  const theme = $("theme").value.trim();
  const flow = $("flow").value;
  if (!theme || !flow) { alert("テーマと議論タイプは必須です"); return; }
  const tags = [...document.querySelectorAll('input[name=tag]:checked')].map(c => c.value);
  const rounds = $("rounds").value.trim() === "" ? undefined : Number($("rounds").value);
  const turnsPerRound = $("turnsPerRound").value.trim() === "" ? undefined : Number($("turnsPerRound").value);
  const opponent = $("opponent").value.trim() || undefined;
  const learningSource = $("learningSource").value || undefined;
  const learningQuery = $("learningQuery").value.trim() || undefined;
  const learningAppId = $("learningAppId").value.trim() === "" ? undefined : Number($("learningAppId").value);
  const learningUrls = $("learningUrls").value.trim() || undefined;
  const specText = $("specText").value.trim() || undefined;
  const specUrl = $("specUrl").value.trim() || undefined;
  $("go").disabled = true;
  const res = await fetch("/api/flow/start", {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ theme, flow, tags, rounds, turnsPerRound, opponent, learningSource, learningQuery, learningAppId, learningUrls, specText, specUrl }),
  }).then(r => r.json()).catch(() => ({ ok: false, error: "通信失敗" }));
  if (!res.ok) { alert(res.error || "開始に失敗"); $("go").disabled = false; return; }
  sessionId = res.sessionId; kind = res.kind;
  if (kind === "learning") {
    $("log").innerHTML = '<div class="u">学習収集 完了: 意見 ' + (res.result?.opinionsRecorded ?? 0) + ' 件 / メカニクス ' + (res.result?.mechanicsRecorded ?? 0) + ' 件 / 自動収集 ' + (res.result?.crawledImported ?? 0) + ' 件</div>';
    $("go").disabled = false;
    return;
  }
  // ペーパーレビューゲート: 草案が ready になるまで待ち、確認・調整 UI を出す。
  if (res.review) { pollPaper(); return; }
  if (kind === "sparring") { $("say").style.display = "block"; }
  poll();
  timer = setInterval(poll, 1500);
});

// ── ペーパー編集 (Notion 風ブロックエディタ・議論開始前) ──
let curPaper = null;       // 最新ドラフト (bodyMd 込み)
const TYPE_LABEL = { heading: "見出し", paragraph: "段落", list: "箇条書き" };

function applyPayload(res) {
  if (res.paper) curPaper = res.paper;
  renderBlocks(res.blocks || []);
  $("rvRevert").disabled = !res.canRevert;
}
function renderBlocks(blocks) {
  const root = $("blocks");
  root.innerHTML = "";
  for (const b of blocks) {
    const div = document.createElement("div");
    div.className = "blk " + b.type;
    div.dataset.id = b.id;
    const rows = Math.min(8, Math.max(1, String(b.text).split("\\n").length));
    div.innerHTML =
      '<div class="blk-type">' + (TYPE_LABEL[b.type] || b.type) + '</div>' +
      '<textarea class="blk-text" rows="' + rows + '"></textarea>' +
      '<div class="blk-actions">' +
        '<button data-act="save" class="primary">保存</button>' +
        '<button data-act="review">LLMレビュー</button>' +
        '<button data-act="crawl">根拠を集める</button>' +
        '<button data-act="del" class="danger">削除</button>' +
      '</div>' +
      '<div class="proposal" style="display:none"></div>';
    div.querySelector(".blk-text").value = b.text;
    root.appendChild(div);
  }
}
function paperApi(path, body) {
  return fetch("/api/flow/" + sessionId + "/paper" + path, {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  }).then(r => r.json()).catch(() => null);
}
async function pollPaper() {
  if (!sessionId) return;
  const res = await fetch("/api/flow/" + sessionId + "/paper").then(r => r.json()).catch(() => null);
  if (!res || !res.ok) { setTimeout(pollPaper, 1500); return; }
  if (!res.ready) { setTimeout(pollPaper, 1500); return; }
  $("review").style.display = "block";
  if (res.error) { $("rvMsg").textContent = "草案作成に失敗: " + res.error; return; }
  if (res.info) $("reviewInfo").textContent = "集めた情報: 外部の声 " + (res.info.voiceCount || 0) + (res.info.countCapped ? "+" : "") + " 件";
  applyPayload(res);
}

// ブロック操作 (イベント委譲)
$("blocks").addEventListener("click", async (e) => {
  const btn = e.target.closest("button[data-act]");
  if (!btn) return;
  const blk = btn.closest(".blk");
  const blockId = blk.dataset.id;
  const act = btn.dataset.act;
  const ta = blk.querySelector(".blk-text");
  if (act === "save") {
    $("rvMsg").textContent = "保存中…";
    const res = await paperApi("/block/apply", { blockId, newText: ta.value, summary: "手編集" });
    if (res && res.ok) { applyPayload(res); $("rvMsg").textContent = "✏️ ブロックを保存"; } else $("rvMsg").textContent = "保存に失敗";
  } else if (act === "del") {
    if (!confirm("このブロックを削除しますか?")) return;
    const res = await paperApi("/block/apply", { blockId, newText: "", summary: "ブロック削除" });
    if (res && res.ok) { applyPayload(res); $("rvMsg").textContent = "🗑 ブロックを削除"; }
  } else if (act === "review") {
    $("rvMsg").textContent = "LLM レビュー中…";
    const inst = prompt("調整方針 (空欄で『議論しやすく明確に』)") || "";
    const res = await paperApi("/block/review", { blockId, instruction: inst });
    const prop = blk.querySelector(".proposal");
    if (!res || !res.ok || !res.reviewed) { $("rvMsg").textContent = "レビュー失敗: " + (res ? res.rationale : ""); return; }
    $("rvMsg").textContent = "";
    prop.style.display = "block";
    prop.innerHTML =
      '<div class="rationale">💡 ' + escapeHtml(res.rationale) + '</div>' +
      '<span class="old">' + escapeHtml(res.original) + '</span>' +
      '<span class="new">' + escapeHtml(res.proposed) + '</span>' +
      '<div class="blk-actions"><button data-act="accept" class="primary">採用</button><button data-act="reject">却下</button></div>';
    prop._proposed = res.proposed;
  } else if (act === "accept") {
    const prop = blk.querySelector(".proposal");
    const res = await paperApi("/block/apply", { blockId, newText: prop._proposed, summary: "LLMレビュー採用" });
    if (res && res.ok) { applyPayload(res); $("rvMsg").textContent = "✅ レビューを採用"; }
  } else if (act === "reject") {
    const prop = blk.querySelector(".proposal");
    prop.style.display = "none"; prop.innerHTML = "";
  } else if (act === "crawl") {
    $("rvMsg").textContent = "根拠を収集中…";
    const res = await paperApi("/crawl", { blockId, insert: false });
    const prop = blk.querySelector(".proposal");
    if (!res || !res.ok) { $("rvMsg").textContent = "収集に失敗"; return; }
    if (!res.evidence || !res.evidence.suggestion) { $("rvMsg").textContent = "根拠となる外部の声が見つかりませんでした"; return; }
    $("rvMsg").textContent = "";
    prop.style.display = "block";
    prop.innerHTML =
      '<div class="rationale">🔎 集めた根拠 (' + (res.evidence.voices.length) + ' 件)</div>' +
      '<span class="new">' + escapeHtml(res.evidence.suggestion) + '</span>' +
      '<div class="blk-actions"><button data-act="insert" class="primary">この根拠を挿入</button><button data-act="reject">閉じる</button></div>';
  } else if (act === "insert") {
    const res = await paperApi("/crawl", { blockId, insert: true });
    if (res && res.ok) { applyPayload(res); $("rvMsg").textContent = "📎 根拠を挿入"; }
  }
});

$("rvEditBtn").addEventListener("click", async () => {
  const instruction = $("rvEdit").value.trim();
  if (!instruction || !sessionId) return;
  $("rvMsg").textContent = "全体調整中…";
  const res = await paperApi("/edit", { instruction });
  if (!res || !res.ok) { $("rvMsg").textContent = "反映に失敗しました"; return; }
  $("rvEdit").value = "";
  applyPayload(res);
  $("rvMsg").textContent = (res.applied ? "✏️ " : "⚠️ ") + (res.changeSummary || "");
});
$("rvRevert").addEventListener("click", async () => {
  const res = await paperApi("/revert", {});
  if (!res || !res.ok) { $("rvMsg").textContent = res ? res.error : "戻せませんでした"; return; }
  applyPayload(res);
  $("rvMsg").textContent = "↶ " + (res.changeSummary || "1 手前に戻しました");
});
$("rvConfirm").addEventListener("change", () => { $("rvApprove").disabled = !$("rvConfirm").checked; });
$("rvApprove").addEventListener("click", async () => {
  if (!sessionId || !curPaper) return;
  const tags = [...document.querySelectorAll('#review .tags input:checked')].map(c => c.value);
  $("rvApprove").disabled = true;
  const res = await paperApi("/approve", { paper: { bodyMd: curPaper.bodyMd, tags } });
  if (!res || !res.ok) { alert("開始に失敗しました"); $("rvApprove").disabled = false; return; }
  $("review").style.display = "none";
  poll();
  timer = setInterval(poll, 1500);
});

$("sayBtn").addEventListener("click", async () => {
  const text = $("sayText").value.trim();
  if (!text || !sessionId) return;
  $("sayText").value = "";
  await fetch("/api/flow/" + sessionId + "/say", {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ text }),
  }).catch(() => {});
  poll();
});

async function poll() {
  if (!sessionId) return;
  const res = await fetch("/api/flow/" + sessionId + "/status?since=" + since).then(r => r.json()).catch(() => null);
  if (!res || !res.ok) return;
  for (const u of res.utterances) {
    since = Math.max(since, u.createdAt);
    const div = document.createElement("div");
    div.className = "u" + (u.role === "user" ? " user" : "") + (u.isError ? " err" : "");
    div.innerHTML = '<div class="who">' + escapeHtml(u.displayName || u.personaName) + '</div>' + escapeHtml(u.text);
    $("log").appendChild(div);
  }
  if (res.conclusion) {
    $("conclusion").style.display = "block";
    $("conclusion").textContent = res.conclusion;
  }
  if (res.done) { clearInterval(timer); $("go").disabled = false; }
}

function escapeHtml(s) { return String(s).replace(/[&<>]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c])); }
</script>
</body>
</html>`;
