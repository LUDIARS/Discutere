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
    <button type="submit" id="go">開始</button>
  </form>

  <div id="say">
    <input id="sayText" type="text" placeholder="壁打ち発話 (「まとめて」「おわり」で操作)" style="width:75%" />
    <button id="sayBtn">送信</button>
  </div>

  <div id="log"></div>
  <div id="conclusion" style="display:none"></div>

<script>
const $ = (id) => document.getElementById(id);
let sessionId = null, kind = null, since = 0, timer = null;

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
  $("go").disabled = true;
  const res = await fetch("/api/flow/start", {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ theme, flow, tags, rounds, turnsPerRound, opponent, learningSource, learningQuery, learningAppId, learningUrls }),
  }).then(r => r.json()).catch(() => ({ ok: false, error: "通信失敗" }));
  if (!res.ok) { alert(res.error || "開始に失敗"); $("go").disabled = false; return; }
  sessionId = res.sessionId; kind = res.kind;
  if (kind === "learning") {
    $("log").innerHTML = '<div class="u">学習収集 完了: 意見 ' + (res.result?.opinionsRecorded ?? 0) + ' 件 / メカニクス ' + (res.result?.mechanicsRecorded ?? 0) + ' 件</div>';
    $("go").disabled = false;
    return;
  }
  if (kind === "sparring") { $("say").style.display = "block"; }
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
