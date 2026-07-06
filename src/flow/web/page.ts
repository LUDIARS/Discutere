/**
 * 簡素な議論フロー WebUI (loopback 1 ページ、依存ゼロの静的 HTML)。
 * テーマ入力 + 議論タイプ (必須) + タグ選択 + 送信。進行はポーリングで表示する。
 */

export const FLOW_HTML = `<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<meta name="color-scheme" content="light dark" />
<title>Discutere 議論フロー</title>
<style>
  :root {
    color-scheme: light;
    --bg: #f8fafc;
    --fg: #111827;
    --muted: #64748b;
    --surface: #ffffff;
    --soft: #eaf2ff;
    --field: #ffffff;
    --border: #d0d7de;
    --primary: #4a90e2;
    --primary-fg: #ffffff;
    --secondary: #e2e8f0;
    --secondary-fg: #334155;
    --danger-bg: #fee2e2;
    --danger-fg: #b91c1c;
    --success: #16a34a;
    --pro-bg: #eff6ff;
    --pro-border: #93c5fd;
    --con-bg: #fff1f2;
    --con-border: #fda4af;
    --neutral-bg: #f8fafc;
    --neutral-border: #cbd5e1;
    --opinion-bg: #f5f3ff;
    --opinion-border: #c4b5fd;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      color-scheme: dark;
      --bg: #0f1115;
      --fg: #e6e6e6;
      --muted: #8b94a3;
      --surface: #161922;
      --soft: #4a90e21a;
      --field: #0c0e13;
      --border: #334155;
      --primary: #4a90e2;
      --primary-fg: #ffffff;
      --secondary: #2a2f3a;
      --secondary-fg: #e5e7eb;
      --danger-bg: #3b1518;
      --danger-fg: #fca5a5;
      --success: #22c55e;
      --pro-bg: #102033;
      --pro-border: #2563eb;
      --con-bg: #32151a;
      --con-border: #be123c;
      --neutral-bg: #111827;
      --neutral-border: #475569;
      --opinion-bg: #211634;
      --opinion-border: #7c3aed;
    }
  }
  * { box-sizing: border-box; }
  body { font-family: system-ui, -apple-system, "Segoe UI", "Hiragino Kaku Gothic ProN", sans-serif; max-width: 1100px; margin: 24px auto; padding: 0 16px 48px; line-height: 1.5; color: var(--fg); background: var(--bg); }
  .app-head { border-bottom: 2px solid var(--primary); padding-bottom: 10px; margin-bottom: 16px; display: flex; align-items: flex-end; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
  h1 { font-size: 22px; margin: 0; }
  .mode-title { display: inline-flex; align-items: center; padding: 4px 12px; border-radius: 999px; background: var(--soft); color: var(--fg); font-weight: 700; font-size: 13px; }
  .panel { border: 1px solid var(--border); border-radius: 8px; padding: 16px; margin: 16px 0; background: var(--surface); }
  .panel h2 { font-size: 16px; margin: 0 0 12px; }
  fieldset { border: 1px solid var(--border); border-radius: 8px; margin: 0 0 12px; background: var(--surface); padding: 12px 14px; }
  legend { color: var(--muted); font-size: 12px; font-weight: 700; padding: 0 6px; }
  label { display: block; margin: 0.4rem 0; }
  input, textarea, select { color: var(--fg); background: var(--surface); }
  input[type=text], input[type=number], textarea, select { background: var(--field); border: 1px solid var(--border); border-radius: 7px; padding: 8px 10px; font: inherit; }
  textarea, select { width: 100%; }
  .tags label { display: inline-block; margin-right: 1rem; }
  button { padding: 8px 15px; border: 1px solid var(--primary); border-radius: 6px; background: var(--primary); color: var(--primary-fg); font: inherit; cursor: pointer; }
  button:disabled { background: #9ca3af; cursor: default; }
  #log { display: grid; gap: 10px; }
  .u { padding: 12px 14px; border: 1px solid var(--neutral-border); border-left-width: 4px; border-radius: 8px; background: var(--neutral-bg); }
  .u.pro { background: var(--pro-bg); border-color: var(--pro-border); }
  .u.con { background: var(--con-bg); border-color: var(--con-border); }
  .u.opinion { background: var(--opinion-bg); border-color: var(--opinion-border); }
  .u.user { border-color: var(--primary); }
  .u .who { font-weight: 700; font-size: 0.9rem; color: var(--fg); display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .u .text { margin-top: 6px; white-space: pre-wrap; word-break: break-word; }
  .err { color: var(--danger-fg); }
  #conclusion { border-left: 4px solid var(--success); }
  #conclusionBody { white-space: pre-wrap; font-weight: 600; word-break: break-word; }
  #say { display: none; margin-top: 1rem; }
  .muted { color: var(--muted); font-size: 0.85rem; }
  .form-grid { display: grid; gap: 12px; }
  .field-note { display: block; color: var(--muted); font-size: 0.78rem; margin-top: 3px; }
  .paper-form { display: grid; gap: 12px; }
  .paper-form textarea { min-height: 86px; }
  .paper-form input[type=text], .paper-form input[type=number] { width: 100%; }
  .settings-row { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; }
  .settings-row label { display: inline-flex; align-items: center; gap: 6px; margin-right: 0; }
  .settings-row input[type=number] { width: 5.5rem; }
  .understanding { display: none; border: 1px solid var(--border); border-left-width: 4px; border-radius: 8px; padding: 10px 12px; margin: 10px 0; background: var(--surface); }
  .understanding.ok { display: block; border-left-color: var(--success); }
  .understanding.warn { display: block; border-left-color: #f59e0b; }
  .understanding strong { display: block; margin-bottom: 4px; }
  .understanding ul { margin: 4px 0 0; padding-left: 1.2rem; }
  .debat { display: none; border: 1px solid var(--border); border-left-width: 4px; border-radius: 8px; padding: 10px 12px; margin: 10px 0; background: var(--surface); }
  .debat.ok { display: block; border-left-color: var(--success); }
  .debat.warn { display: block; border-left-color: #f59e0b; }
  .debat strong { display: block; margin-bottom: 4px; }
  .debat .rec { margin-top: 6px; padding: 8px 10px; background: var(--soft); border-radius: 6px; }
  .suggestion-item { border-top: 1px solid var(--border); padding-top: 10px; margin-top: 10px; }
  .suggestion-item:first-of-type { border-top: 0; padding-top: 0; }
  .suggestion-title { font-weight: 700; margin-bottom: 4px; }
  .suggestion-reason { color: var(--muted); margin-bottom: 6px; }
  .suggestion-fill { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px 10px; margin-top: 8px; }
  .suggestion-fill label { margin: 0; }
  .suggestion-fill input { width: 100%; }
  .suggestion-actions { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin-top: 8px; }
  .suggestion-applied { color: var(--success); font-size: 0.85rem; }
  .learn-panel { display: none; border: 1px solid var(--border); border-left: 4px solid var(--primary); border-radius: 8px; padding: 12px; margin: 10px 0; background: var(--surface); }
  .learn-panel strong { display: block; margin-bottom: 6px; }
  .learn-actions { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 10px; align-items: center; }
  .learn-status { white-space: pre-wrap; }
  .learn-link { display: inline-block; padding: 8px 15px; border: 1px solid var(--primary); border-radius: 6px; background: var(--primary); color: var(--primary-fg); text-decoration: none; }
  .badge.hypo { background: #fae8ff; color: #86198f; }
  #review input[type=text] { width: 100%; }
  #review .row { margin: 0.5rem 0; }
  .blk { background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 12px; margin: 10px 0; }
  .blk.heading { background: var(--soft); }
  .blk .blk-type { font-size: 0.7rem; color: var(--muted); margin-bottom: 0.2rem; }
  .blk .blk-text { width: 100%; resize: vertical; }
  .blk .blk-actions { margin-top: 8px; display: flex; gap: 6px; flex-wrap: wrap; }
  .blk .blk-actions button { padding: 5px 10px; font-size: 12px; background: var(--secondary); color: var(--secondary-fg); border-color: var(--border); }
  .blk .blk-actions button.primary { background: var(--primary); color: var(--primary-fg); }
  .blk .blk-actions button.danger { background: var(--danger-bg); color: var(--danger-fg); }
  .blk .proposal { margin-top: 8px; padding: 10px; background: var(--soft); border-radius: 6px; }
  .blk .proposal .rationale { font-size: 0.8rem; color: var(--muted); margin-bottom: 0.3rem; }
  .blk .proposal .old { background: #fee2e2; text-decoration: line-through; padding: 0.2rem 0.4rem; border-radius: 4px; white-space: pre-wrap; display: block; margin-bottom: 0.2rem; }
  .blk .proposal .new { background: #dcfce7; padding: 0.2rem 0.4rem; border-radius: 4px; white-space: pre-wrap; display: block; }
  .rvbar { margin-top: 12px; display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
  .rvbar input[type=text] { flex: 1; min-width: 14rem; }
  #rvRevert { background: var(--secondary); color: var(--secondary-fg); }
  #rvRevert:disabled { background: var(--soft); color: var(--muted); }
  .confirm { margin-top: 0.8rem; padding-top: 0.6rem; border-top: 1px solid var(--border); display: flex; align-items: center; gap: 0.8rem; }
  #rvApprove { background: var(--success); }
  #rvApprove:disabled { background: #9ca3af; }
  #live { display: grid; grid-template-columns: 1fr; gap: 16px; margin-top: 16px; }
  #rightPanes { display: grid; gap: 16px; min-width: 0; align-content: start; }
  #paperPanel, #opinions, #conclusion { border: 1px solid var(--border); border-radius: 8px; padding: 16px; background: var(--surface); }
  .panel-head { display: flex; justify-content: space-between; align-items: center; gap: 8px; margin-bottom: 10px; }
  #paperPanel h2, #opinions h2, #conclusion h2 { font-size: 16px; margin: 0; }
  #paperToggle { display: none; background: var(--secondary); color: var(--secondary-fg); border-color: var(--border); padding: 5px 10px; font-size: 12px; }
  #paperLive { font-size: 0.75rem; color: var(--primary); }
  .paper-md { font-size: 0.9rem; line-height: 1.55; }
  .paper-md h3 { font-size: 1rem; margin: 0.8rem 0 0.3rem; border-bottom: 1px solid var(--border); padding-bottom: 0.1rem; }
  .paper-md h4 { font-size: 0.9rem; margin: 0.6rem 0 0.2rem; color: var(--fg); }
  .paper-md ul { margin: 0.2rem 0 0.4rem; padding-left: 1.2rem; }
  .paper-md p { margin: 0.3rem 0; }
  .paper-md .upd { animation: flash 1.2s ease-out; }
  @keyframes flash { from { background: #fef9c3; } to { background: transparent; } }
  #newDiscussion { background: var(--success); }
  .listH { font-size: 1rem; margin: 1rem 0 0.4rem; }
  .sess-row { display: flex; align-items: stretch; gap: 0.3rem; margin: 0.3rem 0; }
  .sess { flex: 1; min-width: 0; text-align: left; background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 0.5rem 0.7rem; cursor: pointer; font: inherit; }
  .sess:hover { border-color: var(--primary); background: var(--surface-soft); }
  .sess-del { flex: 0 0 auto; background: var(--danger-bg); color: var(--danger-fg); border: 1px solid var(--border); border-radius: 8px; padding: 0 0.7rem; cursor: pointer; font-size: 1rem; }
  .sess-del:hover { filter: brightness(0.95); }
  .sess .th { font-weight: 600; display: flex; align-items: baseline; gap: 0.45rem; min-width: 0; }
  .sess .dno { flex: 0 0 auto; color: var(--primary); font-weight: 800; font-size: 0.82rem; }
  .sess .ttl { min-width: 0; overflow-wrap: anywhere; }
  .sess .meta { font-size: 0.78rem; color: var(--muted); margin-top: 0.15rem; }
  .badge, .mark { display: inline-block; font-size: 11px; border-radius: 5px; padding: 2px 7px; margin-right: 4px; background: var(--soft); color: var(--fg); font-weight: 700; }
  .mark.vote { background: #fef3c7; color: #92400e; }
  .mark.win { background: #dcfce7; color: #15803d; }
  .mark.auf { background: #ede9fe; color: #6d28d9; }
  .badge.live { background: #dbeafe; color: #1d4ed8; }
  .badge.done { background: #dcfce7; color: #15803d; }
  .badge.draft { background: #fef3c7; color: #b45309; }
  #backBar { margin: 0.6rem 0; }
  #backToList { background: var(--secondary); color: var(--secondary-fg); }
  #filters, #scopeFilters { display: flex; gap: 0.3rem; flex-wrap: wrap; margin: 0.4rem 0; }
  .filt, .scopeFilt { background: var(--secondary); color: var(--secondary-fg); padding: 0.3rem 0.8rem; font-size: 0.85rem; }
  .filt.active, .scopeFilt.active { background: var(--primary); color: var(--primary-fg); }
  #loadMore { display: block; width: 100%; margin: 0.5rem 0; background: var(--secondary); color: var(--secondary-fg); }
  @media (min-width: 960px) {
    body { max-width: 1480px; }
    #live { grid-template-columns: minmax(340px, 1fr) minmax(560px, 2fr); align-items: start; }
    #paperPanel { position: sticky; top: 16px; max-height: calc(100vh - 32px); overflow: auto; }
  }
  @media (max-width: 720px) {
    body { margin: 14px auto; padding: 0 12px 32px; }
    .panel { padding: 12px; }
    .rvbar input[type=text] { min-width: 100%; }
    .suggestion-fill { grid-template-columns: 1fr; }
    #paperToggle { display: inline-block; }
    #paperPanel.collapsed #paperBody { display: none; }
    #paperPanel.collapsed { padding-bottom: 12px; }
  }
</style>
</head>
<body>
  <header class="app-head">
    <h1>Discutere 議論フロー</h1>
    <div class="mode-title">AI議論</div>
  </header>

  <div id="list" class="panel">
    <button id="newDiscussion" type="button">＋ 新規議論開始</button>
    <h2 class="listH">議論一覧</h2>
    <div id="scopeFilters">
      <button class="scopeFilt" data-scope="all" type="button">すべて</button>
      <button class="scopeFilt" data-scope="ai" type="button">AI議論</button>
      <button class="scopeFilt" data-scope="chat" type="button">チャット議論</button>
    </div>
    <div id="filters">
      <button class="filt" data-state="all" type="button">すべて</button>
      <button class="filt" data-state="draft" type="button">下書き</button>
      <button class="filt" data-state="live" type="button">進行中</button>
      <button class="filt" data-state="concluded" type="button">結論あり</button>
    </div>
    <div id="sessionList" class="muted">読み込み中…</div>
    <button id="loadMore" type="button" style="display:none">もっと見る</button>
  </div>

  <div id="backBar" style="display:none"><button id="backToList" type="button">← 議論一覧へ</button></div>

  <form id="start" class="panel" style="display:none">
    <fieldset>
      <legend>ゲームタイトル（または主目的）</legend>
      <input id="gameTitle" type="text" placeholder="例: スイカゲーム / 学校連絡を整理する管理画面" required />
    </fieldset>
    <fieldset>
      <legend>議論したいテーマ</legend>
      <textarea id="discussionTheme" rows="2" placeholder="例: 合体後のテンポを上げるべきか" required></textarea>
    </fieldset>
    <fieldset>
      <legend>議論内容</legend>
      <textarea id="discussionContent" rows="4" placeholder="判断したい論点、比較したい案、避けたい方向性など"></textarea>
    </fieldset>
    <fieldset>
      <legend>議論タイプ (必須)</legend>
      <select id="flow" required>
        <option value="discussion" selected>AI議論</option>
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
      <legend>生成済みペルソナ指定 (任意)</legend>
      <label>ペルソナ名/ID (カンマ区切り) <input id="opponent" type="text" placeholder="例: ローグ好き太郎,ソシャゲ花子" style="width:60%" /></label>
      <span class="field-note">議論/改善では指定ペルソナをキャストに含め、壁打ちでは相手にします。</span>
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
          <option value="reddit">Reddit (OAuth env required)</option>
          <option value="balanced">Balanced: Steam / YouTube / Reddit / Niconico</option>
        </select>
      </label>
      <label>検索クエリ (niconico/youtube・空欄でテーマ) <input id="learningQuery" type="text" placeholder="既定: テーマ文字列" style="width:60%" /></label>
      <label>Steam appId (steam 選択時) <input id="learningAppId" type="number" min="1" placeholder="例: 1145360" style="width:10rem" /></label>
      <label>Reddit subreddit (optional) <input id="learningSubreddit" type="text" placeholder="games" style="width:14rem" /></label>
      <label>URL (website 選択時・カンマ/改行区切り) <input id="learningUrls" type="text" placeholder="https://example.com/article" style="width:60%" /></label>
    </fieldset>
    <fieldset>
      <legend>システム/メカニクスの説明</legend>
      <label>仕様書・設計メモ・既知のメカニクス
        <textarea id="mechanicsContext" rows="6" placeholder="基本ループ、主要システム、操作、報酬、制約、仕様書の抜粋など"></textarea>
      </label>
      <label>ファイルから読み込む (md/txt/json/pdf/docx 等 → 上の欄へ展開)
        <input id="specFile" type="file" accept=".doc,.docx,.txt,.text,.xlsx,.xslx,.pptx,.html,.htm,.pdf,.md,.markdown,.yaml,.yml,.json,text/*" />
        <span id="specFileWarn" style="color:#b91c1c;font-size:0.82rem;display:none"></span>
      </label>
      <label>URL / ローカルパス (取得して解析・貼付本文と結合)
        <input id="specUrl" type="text" placeholder="https://… または spec/feature/foo.md" style="width:80%" />
      </label>
      <label>GitHub repo URL
        <input id="githubRepoUrl" type="text" placeholder="https://github.com/owner/repo" style="width:80%" />
      </label>
      <label>GitHub file path / ref
        <input id="githubPath" type="text" placeholder="docs/spec.md" style="width:52%" />
        <input id="githubRef" type="text" placeholder="main" style="width:18%" />
        <button id="githubCheck" type="button">Check GitHub read</button>
      </label>
      <pre id="githubAuth" class="muted" style="white-space:pre-wrap;margin:4px 0 0"></pre>
      <div id="githubCheckMsg" class="muted"></div>
    </fieldset>
    <fieldset>
      <legend>Anatomia プロジェクトコード (任意)</legend>
      <label>登録済みプロジェクト名 <input id="anatomiaProject" type="text" placeholder="例: pagus (anatomia project add 済み)" style="width:50%" /></label>
      <label>または リポジトリ絶対パス <input id="anatomiaRepo" type="text" placeholder="例: E:/Document/Ars/Pagus" style="width:60%" /></label>
      <p class="muted" style="margin:4px 0 0;font-size:0.82rem">サーバ設定で Anatomia 連携が有効な場合のみ反映。ドメイン下地が無ければ初回は自動解析で時間がかかります。</p>
    </fieldset>
    <fieldset>
      <legend>テーマについての補足情報</legend>
      <textarea id="themeSupplement" rows="3" placeholder="前提条件、対象ユーザ、禁止事項、評価軸など"></textarea>
    </fieldset>
    <button type="submit" id="go">開始</button>
  </form>

  <div id="say">
    <input id="sayText" type="text" placeholder="壁打ち発話 (「まとめて」「おわり」で操作)" style="width:75%" />
    <button id="sayBtn">送信</button>
  </div>

  <div id="review" class="panel" style="display:none">
    <h2>📝 ディスカッションペーパー編集 (確定すると議論開始できます)</h2>
    <div id="reviewInfo" class="muted">準備中…</div>
    <div id="understanding" class="understanding"></div>
    <div id="debatability" class="debat"></div>
    <div id="voiceSimulation" class="understanding"></div>
    <div id="fixSuggestions" class="debat"></div>
    <div id="mechanicsKnowledge" class="understanding"></div>
    <div id="reviewLearning" class="learn-panel">
      <strong>追加学習</strong>
      <div class="muted">学習不足が指摘されています。追加学習は学習ページで実行します。</div>
      <div class="learn-actions">
        <a id="rvLearningLink" class="learn-link" href="/learning" target="_blank" rel="noreferrer">追加学習ページを開く</a>
        <span id="rvLearningStatus" class="muted learn-status">学習後、この画面で再度「議論可能か確認する」を実行してください。</span>
      </div>
    </div>
    <div id="fixedPaper" class="paper-form">
      <label>ゲームタイトル（または主目的）
        <input id="rvGameTitle" type="text" />
      </label>
      <label>議論したいテーマ
        <textarea id="rvDiscussionTheme" rows="2"></textarea>
      </label>
      <label>議論内容
        <textarea id="rvDiscussionContent" rows="4"></textarea>
      </label>
      <label>論点 (1行1論点・議論適性ゲートの分解結果を編集できます)
        <textarea id="rvIssues" rows="4" placeholder="例: ガチャ緩和は長期収益を毀損するか"></textarea>
        <span class="field-note">承認時にファシリテーターの参考論点として渡されます。空にすると論点なしで開始します。</span>
      </label>
      <label>システム/メカニクスの説明
        <textarea id="rvMechanicsContext" rows="6"></textarea>
        <span class="field-note">仕様書、Anatomia由来のコード理解、抽出済みメカニクスをここで確認・補足します。</span>
      </label>
      <label>テーマについての補足情報
        <textarea id="rvThemeSupplement" rows="3"></textarea>
      </label>
      <div class="row tags">タグ:
        <label><input data-review-tag type="checkbox" value="機密" /> 機密</label>
        <label><input data-review-tag type="checkbox" value="内部" /> 内部</label>
        <label><input data-review-tag type="checkbox" value="運用" /> 運用</label>
        <label><input data-review-tag type="checkbox" value="開発" /> 開発</label>
      </div>
      <div class="settings-row">
        <label>ラウンド数 <input id="rvRounds" type="number" min="1" max="10" placeholder="既定" /></label>
        <label>ターン数/ラウンド <input id="rvTurnsPerRound" type="number" min="1" max="20" placeholder="既定" /></label>
        <button id="rvSaveFields" type="button">内容を保存</button>
        <button id="rvCheckDebate" type="button">議論可能か確認する</button>
      </div>
    </div>
    <div id="blocks" style="display:none"></div>
    <div class="rvbar">
      <button id="rvRefreshSuggestions" type="button">指摘内容の修正提案</button>
      <button id="rvMechanicsCheck" type="button">メカニクス知識を確認</button>
      <button id="rvRevert" type="button" disabled>↶ 戻す</button>
    </div>
    <div id="rvMsg" class="muted"></div>
    <div class="confirm">
      <label><input type="checkbox" id="rvConfirm" /> ペーパーを確定する</label>
      <button id="rvApprove" type="button" disabled>議論開始</button>
    </div>
  </div>

  <div id="live" style="display:none">
    <div id="paperPanel">
      <div class="panel-head"><h2>ディスカッションペーパー <span id="paperLive" class="muted"></span></h2><button id="paperToggle" type="button">閉じる</button></div>
      <div id="paperBody" class="paper-md"></div>
    </div>
    <div id="rightPanes">
      <div id="opinions">
        <div class="panel-head"><h2>各LLMの意見議論内容</h2></div>
        <div id="log"></div>
      </div>
      <div id="conclusion" style="display:none">
        <div class="panel-head"><h2>収束結果</h2></div>
        <div id="conclusionBody"></div>
      </div>
    </div>
  </div>

<script>
const $ = (id) => document.getElementById(id);
let sessionId = null, kind = null, since = 0, timer = null;
let paperReadyNotifiedFor = null;

function requestPaperNotificationPermission() {
  if (!("Notification" in window)) return;
  if (Notification.permission === "default") Notification.requestPermission().catch(() => {});
}
function notifyPaperReady(message) {
  if (!sessionId || paperReadyNotifiedFor === sessionId) return;
  paperReadyNotifiedFor = sessionId;
  if (!("Notification" in window) || Notification.permission !== "granted") return;
  new Notification("Discutere", { body: message || "Discussion paper is ready.", tag: "discutere-paper-" + sessionId });
}

// 仕様書ファイル選択: 対応形式はサーバ抽出エンドポイント経由で mechanicsContext 欄へ展開する。
const SPEC_FILE_MAX_BYTES = 5 * 1024 * 1024; // 5MB
const SPEC_TEXT_MAX_CHARS = 200_000;         // 200k 文字警告
const SPEC_EXTS = new Set([".doc", ".docx", ".txt", ".text", ".xlsx", ".xslx", ".pptx", ".html", ".htm", ".pdf", ".md", ".markdown", ".yaml", ".yml", ".json"]);
function specExt(name) {
  const i = name.lastIndexOf(".");
  return i >= 0 ? name.slice(i).toLowerCase() : "";
}
function isSupportedSpec(name) {
  return SPEC_EXTS.has(specExt(name));
}
function showSpecWarn(msg) {
  const el = $("specFileWarn");
  if (!el) return;
  el.textContent = msg;
  el.style.display = msg ? "block" : "none";
}
function appendSpecText(text) {
  const cur = $("mechanicsContext").value.trim();
  $("mechanicsContext").value = cur ? (cur + "\\n\\n" + text) : text;
  if (text.length > SPEC_TEXT_MAX_CHARS) {
    showSpecWarn("テキストが長いです (" + text.length.toLocaleString() + " 文字)。サーバ側でチャンク分割して解析します。");
  } else {
    showSpecWarn("");
  }
}
async function loadGithubAuth() {
  const el = $("githubAuth");
  if (!el) return;
  const res = await fetch("/api/spec/github/auth").then(r => r.json()).catch(() => null);
  if (!res || !res.ok) { el.textContent = "gh auth status unavailable"; return; }
  el.textContent = res.auth && res.auth.output ? res.auth.output : "gh auth status unavailable";
}
async function checkGithubRead() {
  const msg = $("githubCheckMsg");
  msg.textContent = "Checking GitHub read...";
  const body = {
    repoUrl: $("githubRepoUrl").value.trim(),
    path: $("githubPath").value.trim(),
    ref: $("githubRef").value.trim(),
  };
  const res = await fetch("/api/spec/github/check", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  }).then(r => r.json()).catch(() => null);
  if (!res) { msg.textContent = "GitHub check failed"; return; }
  if (!res.ok) { msg.textContent = res.error || "GitHub check failed"; return; }
  msg.textContent = res.readable ? "GitHub read OK" : "GitHub read failed";
  if (res.text) appendSpecText(res.text);
  if (res.auth && res.auth.output) $("githubAuth").textContent = res.auth.output;
}
$("specFile").addEventListener("change", async (e) => {
  const file = e.target.files && e.target.files[0];
  if (!file) return;
  showSpecWarn("");
  if (!isSupportedSpec(file.name)) {
    showSpecWarn("Unsupported file format: " + file.name);
    e.target.value = "";
    return;
  }
  if (file.size > SPEC_FILE_MAX_BYTES) {
    const mb = (file.size / 1024 / 1024).toFixed(1);
    showSpecWarn("ファイルが大きいです (" + mb + " MB)。処理に時間がかかる場合があります。");
  }
  const fd = new FormData();
  fd.append("file", file);
  $("go").disabled = true;
  try {
    const res = await fetch("/api/spec/extract", { method: "POST", body: fd }).then(r => r.json());
    if (!res.ok) { showSpecWarn("抽出エラー: " + (res.error || "不明")); return; }
    appendSpecText(res.text);
  } catch {
    showSpecWarn("ファイルの送信に失敗しました");
  } finally {
    $("go").disabled = false;
  }
});

$("start").addEventListener("submit", async (e) => {
  e.preventDefault();
  requestPaperNotificationPermission();
  const gameTitle = $("gameTitle").value.trim();
  const discussionTheme = $("discussionTheme").value.trim();
  const discussionContent = $("discussionContent").value.trim() || undefined;
  const mechanicsContext = $("mechanicsContext").value.trim() || undefined;
  const themeSupplement = $("themeSupplement").value.trim() || undefined;
  const theme = [gameTitle, discussionTheme].filter(Boolean).join(" / ");
  const flow = $("flow").value;
  if (!gameTitle || !discussionTheme || !flow) { alert("ゲームタイトル(または主目的)、議論したいテーマ、議論タイプは必須です"); return; }
  const tags = [...document.querySelectorAll('input[name=tag]:checked')].map(c => c.value);
  const rounds = $("rounds").value.trim() === "" ? undefined : Number($("rounds").value);
  const turnsPerRound = $("turnsPerRound").value.trim() === "" ? undefined : Number($("turnsPerRound").value);
  const personaIds = $("opponent").value.trim() || undefined;
  const learningSource = $("learningSource").value || undefined;
  const learningQuery = $("learningQuery").value.trim() || undefined;
  const learningAppId = $("learningAppId").value.trim() === "" ? undefined : Number($("learningAppId").value);
  const learningSubreddit = $("learningSubreddit").value.trim() || undefined;
  const learningUrls = $("learningUrls").value.trim() || undefined;
  const learningBalanced = learningSource === "balanced" ? true : undefined;
  const specText = mechanicsContext;
  const specUrl = $("specUrl").value.trim() || undefined;
  const githubRepoUrl = $("githubRepoUrl").value.trim() || undefined;
  const githubPath = $("githubPath").value.trim() || undefined;
  const githubRef = $("githubRef").value.trim() || undefined;
  const anatomiaProject = $("anatomiaProject").value.trim() || undefined;
  const anatomiaRepo = $("anatomiaRepo").value.trim() || undefined;
  $("go").disabled = true;
  const res = await fetch("/api/flow/start", {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ theme, gameTitle, discussionTheme, discussionContent, mechanicsContext, themeSupplement, flow, tags, rounds, turnsPerRound, personaIds, opponent: personaIds, learningSource, learningQuery, learningAppId, learningSubreddit, learningUrls, learningBalanced, specText, specUrl, githubRepoUrl, githubPath, githubRef, anatomiaProject, anatomiaRepo }),
  }).then(r => r.json()).catch(() => ({ ok: false, error: "通信失敗" }));
  if (!res.ok) { alert(res.error || "開始に失敗"); $("go").disabled = false; return; }
  sessionId = res.sessionId; kind = res.kind;
  // 開始したらフォームは隠す (各モードの画面へ遷移する)。
  $("start").style.display = "none";
  if (kind === "learning") {
    $("live").style.display = "grid"; $("paperPanel").style.display = "none"; $("backBar").style.display = "block";
    $("log").innerHTML = '<div class="u">学習収集 完了: 意見 ' + (res.result?.opinionsRecorded ?? 0) + ' 件 / メカニクス ' + (res.result?.mechanicsRecorded ?? 0) + ' 件 / 自動収集 ' + (res.result?.crawledImported ?? 0) + ' 件</div>';
    $("go").disabled = false;
    return;
  }
  // ペーパー編集ゲート: 即座に確認画面へ遷移し「準備中」を出す。草案が ready になったら編集 UI を埋める。
  if (res.review) {
    $("review").style.display = "block";
    $("backBar").style.display = "block";
    $("reviewInfo").textContent = "ペーパーを準備中… (情報収集・草案作成)";
    $("fixedPaper").style.display = "none";
    $("blocks").innerHTML = '<div class="muted">📝 ディスカッションペーパーを作成しています。少々お待ちください…</div>';
    $("rvApprove").disabled = true;
    window.scrollTo(0, 0);
    pollPaper();
    return;
  }
  if (kind === "sparring") { $("say").style.display = "block"; }
  startLive();
});

// ── ペーパー編集 (Notion 風ブロックエディタ・議論開始前) ──
let curPaper = null;       // 最新ドラフト (bodyMd 込み)
let paperPollMisses = 0;
const TYPE_LABEL = { heading: "見出し", paragraph: "段落", list: "箇条書き" };

function applyPayload(res) {
  if (res.paper) curPaper = res.paper;
  renderFixedForm(res);
  $("rvRevert").disabled = !res.canRevert;
}
function collectReviewTags() {
  return [...document.querySelectorAll('#review input[data-review-tag]:checked')].map(c => c.value);
}
function setReviewTags(tags) {
  const values = new Set(tags || []);
  for (const c of document.querySelectorAll('#review input[data-review-tag]')) c.checked = values.has(c.value);
}
function setInputValue(id, value) {
  const el = $(id);
  if (document.activeElement === el) return;
  el.value = value || "";
}
function renderUnderstanding(info) {
  const el = $("understanding");
  const u = info && info.understanding;
  if (!u) { el.style.display = "none"; el.className = "understanding"; el.innerHTML = ""; return; }
  el.className = "understanding " + (u.ok ? "ok" : "warn");
  const qs = (u.missingQuestions || []).map(q => "<li>" + escapeHtml(q) + "</li>").join("");
  el.innerHTML = "<strong>AI理解確認: " + (u.ok ? "基本的なゲーム内容を把握できています" : "補足が必要です") + "</strong>" +
    "<div>" + escapeHtml(u.rationale || "") + "</div>" +
    (qs ? "<ul>" + qs + "</ul>" : "");
}
function renderReviewLearning(d) {
  const panel = $("reviewLearning");
  if (!d || d.degraded || d.debatable || !d.recommendation || d.recommendation.flow !== "learning") {
    panel.style.display = "none";
    return;
  }
  panel.style.display = "block";
  const p = new URLSearchParams();
  const gameTitle = $("rvGameTitle").value.trim();
  const theme = $("rvDiscussionTheme").value.trim();
  if (gameTitle) p.set("gameTitle", gameTitle);
  if (theme) p.set("theme", theme);
  p.set("query", [gameTitle, theme, "review"].filter(Boolean).join(" "));
  p.set("source", "balanced");
  $("rvLearningLink").href = "/learning?" + p.toString();
}
function renderDebatability(info) {
  const el = $("debatability");
  const d = info && info.debatability;
  renderReviewLearning(d);
  if (!d) { el.style.display = "none"; el.className = "debat"; el.innerHTML = ""; return; }
  if (d.degraded) {
    el.className = "debat warn";
    el.innerHTML = "<strong>議論適性チェック: 判定を完了できませんでした</strong>" +
      "<div>" + escapeHtml(d.message || "LLMチェックに失敗しました。内容は保存されています。") + "</div>";
    el.style.display = "block";
    return;
  }
  el.className = "debat " + (d.debatable ? "ok" : "warn");
  let html = "<strong>議論適性ゲート: " + (d.debatable ? "議論に向いています" : "議論不適 (再提案あり)") + "</strong>" +
    "<div>" + escapeHtml(d.message || "") + "</div>";
  if (!d.debatable && d.recommendation) {
    const label = d.recommendation.flow === "sparring" ? "壁打ち" : "学習";
    html += '<div class="rec">💡 <strong>提案: 「' + label + '」フロー</strong>' +
      escapeHtml(d.recommendation.reason || "") +
      '<div class="muted">このまま下の「議論開始」で強行することもできます (人間が最終決定)。</div></div>';
  }
  el.innerHTML = html;
  el.style.display = "block";
}
function renderVoiceSimulation(info) {
  const el = $("voiceSimulation");
  const v = info && info.voiceSimulation;
  if (!v) { el.style.display = "none"; el.className = "understanding"; el.innerHTML = ""; return; }
  el.className = "understanding " + (v.possible ? "ok" : "warn");
  el.innerHTML = "<strong>ユーザの声のLLM生成シミュレーション: " +
    (v.possible ? "可能" : "先に補強推奨") + " / " + escapeHtml(v.confidence || "low") + "</strong>" +
    "<div>" + escapeHtml(v.summary || "") + "</div>" +
    (v.nextAction ? '<div class="rec">' + escapeHtml(v.nextAction) + "</div>" : "") +
    (v.caveat ? '<div class="muted">' + escapeHtml(v.caveat) + "</div>" : "");
  el.style.display = "block";
}
function suggestionPlaceholders(template) {
  const re = /\\{\\{([^{}\\n]{1,80})\\}\\}|\\[([^\\[\\]\\n]{1,80})\\]|＜([^＜＞\\n]{1,80})＞|<([^<>\\n]{1,80})>/g;
  const found = [];
  const byKey = new Map();
  let m;
  while ((m = re.exec(template))) {
    const label = (m[1] || m[2] || m[3] || m[4] || "").trim();
    if (!label || label.startsWith("http://") || label.startsWith("https://")) continue;
    const key = label.toLowerCase();
    let item = byKey.get(key);
    if (!item) {
      item = { id: "ph" + found.length, label, raws: [] };
      byKey.set(key, item);
      found.push(item);
    }
    if (!item.raws.includes(m[0])) item.raws.push(m[0]);
  }
  return found.slice(0, 8);
}
function fillSuggestionTemplate(item, requireAll) {
  let text = item._template || "";
  const placeholders = item._placeholders || [];
  for (const ph of placeholders) {
    const input = item.querySelector('input[data-ph-id="' + ph.id + '"]');
    const value = input ? input.value.trim() : "";
    if (!value && requireAll) return { ok: false, missing: input, text };
    for (const raw of ph.raws) text = text.split(raw).join(value || raw);
  }
  return { ok: true, text: text.trim() };
}
function updateSuggestionPreview(item) {
  const preview = item.querySelector(".suggestion-preview");
  if (!preview) return;
  const filled = fillSuggestionTemplate(item, false);
  preview.textContent = filled.text || item._template || "";
}
function renderFixSuggestions(info) {
  const el = $("fixSuggestions");
  const suggestions = (info && info.fixSuggestions) || [];
  if (!suggestions.length) { el.style.display = "none"; el.className = "debat"; el.innerHTML = ""; return; }
  el.className = "debat warn";
  el.innerHTML = "";
  const heading = document.createElement("strong");
  heading.textContent = "指摘内容の修正提案";
  el.appendChild(heading);
  for (let i = 0; i < suggestions.length; i++) {
    const s = suggestions[i] || {};
    const item = document.createElement("div");
    item.className = "suggestion-item";
    item.dataset.index = String(i);
    item._template = String(s.appliedText || s.suggestedChange || "");
    item._placeholders = s.appliedAt ? [] : suggestionPlaceholders(item._template);

    const title = document.createElement("div");
    title.className = "suggestion-title";
    title.textContent = s.title || "修正提案";
    item.appendChild(title);
    if (s.reason) {
      const reason = document.createElement("div");
      reason.className = "suggestion-reason";
      reason.textContent = s.reason;
      item.appendChild(reason);
    }
    if (item._placeholders.length) {
      const fill = document.createElement("div");
      fill.className = "suggestion-fill";
      for (const ph of item._placeholders) {
        const label = document.createElement("label");
        label.textContent = ph.label;
        const input = document.createElement("input");
        input.type = "text";
        input.placeholder = ph.label;
        input.dataset.phId = ph.id;
        label.appendChild(input);
        fill.appendChild(label);
      }
      item.appendChild(fill);
    }
    const preview = document.createElement("div");
    preview.className = "rec suggestion-preview";
    item.appendChild(preview);
    const actions = document.createElement("div");
    actions.className = "suggestion-actions";
    const applyBtn = document.createElement("button");
    applyBtn.type = "button";
    applyBtn.dataset.act = "apply-fix";
    applyBtn.textContent = s.appliedAt ? "反映済み" : "調整";
    applyBtn.disabled = !!s.appliedAt;
    actions.appendChild(applyBtn);
    if (s.appliedAt) {
      const applied = document.createElement("span");
      applied.className = "suggestion-applied";
      applied.textContent = "本文に追記済み";
      actions.appendChild(applied);
    } else if (item._placeholders.length) {
      const help = document.createElement("span");
      help.className = "muted";
      help.textContent = "空欄を埋めると追記文に反映されます。";
      actions.appendChild(help);
    }
    item.appendChild(actions);
    el.appendChild(item);
    updateSuggestionPreview(item);
  }
  el.style.display = "block";
}
function renderMechanicsKnowledge(info) {
  const el = $("mechanicsKnowledge");
  const k = info && info.mechanicsKnowledge;
  if (!k) { el.style.display = "none"; el.className = "understanding"; el.innerHTML = ""; return; }
  el.className = "understanding " + (k.ok ? "ok" : "warn");
  const known = (k.knownMechanics || []).map(v => "<li>" + escapeHtml(v) + "</li>").join("");
  const missing = (k.missingQuestions || []).map(v => "<li>" + escapeHtml(v) + "</li>").join("");
  el.innerHTML = "<strong>メカニクス知識確認: " + escapeHtml(k.confidence || "low") + "</strong>" +
    "<div>" + escapeHtml(k.summary || "") + "</div>" +
    (known ? '<div class="muted">把握している内容</div><ul>' + known + "</ul>" : "") +
    (missing ? '<div class="muted">不足している確認事項</div><ul>' + missing + "</ul>" : "");
  el.style.display = "block";
}
function renderFixedForm(res) {
  $("fixedPaper").style.display = "grid";
  const f = res.fixedFields || {};
  setInputValue("rvGameTitle", f.gameTitle);
  setInputValue("rvDiscussionTheme", f.discussionTheme || (curPaper && curPaper.theme) || "");
  setInputValue("rvDiscussionContent", f.discussionContent);
  setInputValue("rvIssues", ((curPaper && curPaper.issues) || []).join("\\n"));
  setInputValue("rvMechanicsContext", f.mechanicsContext);
  setInputValue("rvThemeSupplement", f.themeSupplement || (curPaper && curPaper.supplement) || "");
  setReviewTags((curPaper && curPaper.tags) || []);
  const settings = res.settings || {};
  setInputValue("rvRounds", settings.rounds == null ? "" : settings.rounds);
  setInputValue("rvTurnsPerRound", settings.turnsPerRound == null ? "" : settings.turnsPerRound);
  renderUnderstanding(res.info);
  renderDebatability(res.info);
  renderVoiceSimulation(res.info);
  renderFixSuggestions(res.info);
  renderMechanicsKnowledge(res.info);
}
function collectFixedForm() {
  return {
    gameTitle: $("rvGameTitle").value.trim(),
    discussionTheme: $("rvDiscussionTheme").value.trim(),
    discussionContent: $("rvDiscussionContent").value.trim(),
    issues: $("rvIssues").value,
    mechanicsContext: $("rvMechanicsContext").value.trim(),
    themeSupplement: $("rvThemeSupplement").value.trim(),
    tags: collectReviewTags(),
    rounds: $("rvRounds").value.trim() === "" ? undefined : Number($("rvRounds").value),
    turnsPerRound: $("rvTurnsPerRound").value.trim() === "" ? undefined : Number($("rvTurnsPerRound").value),
  };
}
async function saveFixedForm(message) {
  const form = collectFixedForm();
  if (!form.gameTitle || !form.discussionTheme) {
    $("rvMsg").textContent = "ゲームタイトル(または主目的)と議論したいテーマは必須です";
    return null;
  }
  $("rvMsg").textContent = message || "保存中…";
  const res = await paperApi("/form/apply", form);
  if (!res || !res.ok) {
    $("rvMsg").textContent = res && res.error ? res.error : "保存に失敗しました";
    return null;
  }
  applyPayload(res);
  $("rvMsg").textContent = "保存しました";
  return res;
}
function renderBlocks(blocks) {
  const root = $("blocks");
  root.innerHTML = "";
  for (const b of blocks) {
    const div = document.createElement("div");
    div.className = "blk " + b.type;
    div.dataset.id = b.id;
    const rows = Math.min(8, Math.max(1, String(b.text).split("\\n").length));
    // 仮説メカニクス節 (LLM抽出・未検証) の見出しブロックにはバッジを出す (08)。
    const hypoBadge = b.type === "heading" && String(b.text).includes("仮説メカニクス")
      ? '<span class="badge hypo">🧪 未検証 (LLM抽出)</span>'
      : "";
    div.innerHTML =
      '<div class="blk-type">' + (TYPE_LABEL[b.type] || b.type) + hypoBadge + '</div>' +
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
  if (res.started) {
    paperPollMisses = 0;
    $("review").style.display = "none";
    startLive();
    return;
  }
  if (res.missing) {
    paperPollMisses += 1;
    if (paperPollMisses < 3) { setTimeout(pollPaper, 1500); return; }
    $("reviewInfo").textContent = "ペーパー準備状態が見つかりません。議論一覧から開き直してください。";
    $("blocks").innerHTML = '<div class="muted">準備中のセッションが見つかりませんでした。</div>';
    return;
  }
  paperPollMisses = 0;
  if (!res.ready) { setTimeout(pollPaper, 1500); return; }
  notifyPaperReady(res.error ? "Discussion paper generation finished with an error." : "Discussion paper is ready.");
  $("review").style.display = "block";
  if (res.error) { $("rvMsg").textContent = "草案作成に失敗: " + res.error; return; }
  if (res.info) {
    let infoText = "集めた情報: 外部の声 " + (res.info.voiceCount || 0) + (res.info.countCapped ? "+" : "") + " 件";
    const hypo = ((res.paper && res.paper.mechanics) || []).filter((m) => m.source === "llm").length;
    if (hypo > 0) infoText += " / 🧪 仮説メカニクス " + hypo + " 件 (LLM抽出・未検証)";
    $("reviewInfo").textContent = infoText;
  }
  applyPayload(res);
}

$("fixSuggestions").addEventListener("input", (e) => {
  const input = e.target.closest("input[data-ph-id]");
  if (!input) return;
  const item = input.closest(".suggestion-item");
  if (item) updateSuggestionPreview(item);
});
$("fixSuggestions").addEventListener("click", async (e) => {
  const btn = e.target.closest('button[data-act="apply-fix"]');
  if (!btn || btn.disabled || !sessionId) return;
  const item = btn.closest(".suggestion-item");
  if (!item) return;
  const filled = fillSuggestionTemplate(item, true);
  if (!filled.ok) {
    $("rvMsg").textContent = "未入力のプレースホルダがあります";
    if (filled.missing) filled.missing.focus();
    return;
  }
  const suggestionIndex = Number(item.dataset.index);
  btn.disabled = true;
  const saved = await saveFixedForm("調整前に保存中…");
  if (!saved) { btn.disabled = false; return; }
  $("rvMsg").textContent = "修正提案を追記中…";
  const res = await paperApi("/fix-suggestion/apply", { suggestionIndex, text: filled.text });
  if (!res || !res.ok) {
    $("rvMsg").textContent = res && res.error ? res.error : "修正提案の追記に失敗しました";
    return;
  }
  applyPayload(res);
  $("rvMsg").textContent = "修正提案を追記しました";
});

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

$("rvSaveFields").addEventListener("click", () => { void saveFixedForm("保存中…"); });

async function runDebatabilityCheck() {
  if (!sessionId) return;
  const saved = await saveFixedForm("確認前に保存中…");
  if (!saved) return;
  $("rvCheckDebate").disabled = true;
  $("rvRefreshSuggestions").disabled = true;
  $("rvMsg").textContent = "議論可能性を確認中…";
  const res = await paperApi("/debatability/check", {});
  $("rvCheckDebate").disabled = false;
  $("rvRefreshSuggestions").disabled = false;
  if (!res || !res.ok) { $("rvMsg").textContent = res && res.error ? res.error : "確認に失敗しました"; return; }
  applyPayload(res);
  $("rvMsg").textContent = "議論可能性を確認しました";
}
async function runMechanicsKnowledgeCheck() {
  if (!sessionId) return;
  const saved = await saveFixedForm("確認前に保存中…");
  if (!saved) return;
  $("rvMechanicsCheck").disabled = true;
  $("rvMsg").textContent = "メカニクス知識を確認中…";
  const res = await paperApi("/mechanics/check", {});
  $("rvMechanicsCheck").disabled = false;
  if (!res || !res.ok) { $("rvMsg").textContent = res && res.error ? res.error : "確認に失敗しました"; return; }
  applyPayload(res);
  $("rvMsg").textContent = "メカニクス知識を確認しました";
}
$("rvCheckDebate").addEventListener("click", () => { void runDebatabilityCheck(); });
$("rvRefreshSuggestions").addEventListener("click", () => { void runDebatabilityCheck(); });
$("rvMechanicsCheck").addEventListener("click", () => { void runMechanicsKnowledgeCheck(); });
$("rvRevert").addEventListener("click", async () => {
  const res = await paperApi("/revert", {});
  if (!res || !res.ok) { $("rvMsg").textContent = res ? res.error : "戻せませんでした"; return; }
  applyPayload(res);
  $("rvMsg").textContent = "↶ " + (res.changeSummary || "1 手前に戻しました");
});
$("rvConfirm").addEventListener("change", () => { $("rvApprove").disabled = !$("rvConfirm").checked; });
$("rvApprove").addEventListener("click", async () => {
  if (!sessionId || !curPaper) return;
  $("rvApprove").disabled = true;
  const saved = await saveFixedForm("開始前に保存中…");
  if (!saved) { $("rvApprove").disabled = false; return; }
  const tags = collectReviewTags();
  const res = await paperApi("/approve", { paper: { bodyMd: curPaper.bodyMd, tags } });
  if (!res || !res.ok) { alert("開始に失敗しました"); $("rvApprove").disabled = false; return; }
  $("review").style.display = "none";
  startLive();
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

// 議論ライブ表示を開始する (ペーパー + 各 LLM の意見の 2 ペイン)。
function startLive() {
  $("live").style.display = "grid";
  $("backBar").style.display = "block";
  poll();
  timer = setInterval(poll, 1500);
}

// ── 議論一覧 / ナビゲーション ──
const PAGE = 50;               // 1 ページの取得件数
const initialScope = new URLSearchParams(location.search).get("scope");
let curScope = initialScope === "ai" || initialScope === "chat" ? initialScope : "all";
let curState = "all", curOffset = 0;

// 1 セッション行 (本体ボタン + 削除ボタン) を組んで返す。
function buildSessionRow(s) {
  const row = document.createElement("div");
  row.className = "sess-row";
  const state = s.state || (s.concluded ? "concluded" : "live");
  const badge = s.state === "draft"
    ? '<span class="badge draft">下書き(編集中)</span>'
    : s.concluded
      ? '<span class="badge done">結論あり</span>'
      : '<span class="badge live">進行/未収束</span>';
  const when = new Date(s.createdAt).toLocaleString();
  const discussionNo = Number.isFinite(s.discussionNo) && s.discussionNo > 0 ? "#" + s.discussionNo : "#?";
  const b = document.createElement("button");
  b.className = "sess"; b.dataset.sid = s.sessionId; b.dataset.state = state; b.type = "button";
  b.innerHTML = '<div class="th"><span class="dno">' + escapeHtml(discussionNo) + '</span><span class="ttl">' + escapeHtml(s.title || s.theme || "(無題)") + '</span></div>' +
    '<div class="meta">' + badge + escapeHtml(s.flow) + ' / 発話 ' + s.utterances + ' 件 / ' + when + '</div>';
  const del = document.createElement("button");
  del.className = "sess-del"; del.dataset.del = s.sessionId; del.type = "button";
  del.title = "この議論を削除"; del.textContent = "🗑";
  row.appendChild(b); row.appendChild(del);
  return row;
}

// 議論一覧を読み込む。reset=true で先頭から (フィルタ切替/削除後)、false で追加読み込み (もっと見る)。
async function loadSessions(reset = true) {
  const el = $("sessionList");
  if (reset) { curOffset = 0; el.innerHTML = ""; el.classList.add("muted"); el.textContent = "読み込み中…"; }
  const q = "scope=" + curScope + "&state=" + curState + "&limit=" + PAGE + "&offset=" + curOffset;
  const res = await fetch("/api/flow/sessions?" + q).then(r => r.json()).catch(() => null);
  if (!res || !res.ok) { if (reset) { el.textContent = "一覧の取得に失敗しました"; } return; }
  if (reset) { el.innerHTML = ""; el.classList.remove("muted"); }
  if (reset && !res.sessions.length) {
    el.classList.add("muted");
    el.innerHTML = curState === "all"
      ? '<div class="muted">まだ議論はありません。「＋ 新規議論開始」から始めてください。</div>'
      : '<div class="muted">この絞り込みに該当する議論はありません。</div>';
  }
  for (const s of res.sessions) el.appendChild(buildSessionRow(s));
  curOffset += res.sessions.length;
  $("loadMore").style.display = res.hasMore ? "block" : "none";
}
// 議論を削除する (下書き/不要議論の破棄)。確定後に一覧を再読み込み。
async function deleteSession(sid, theme) {
  if (!confirm("「" + (theme || "(無題)") + "」を削除しますか? (元に戻せません)")) return;
  const res = await fetch("/api/flow/" + sid, { method: "DELETE" }).then(r => r.json()).catch(() => null);
  if (!res || !res.ok) { alert(res && res.error ? res.error : "削除に失敗しました"); return; }
  loadSessions();
}
// 永続済みドラフトの編集を再開する (一覧の「下書き」クリック)。
function openDraft(sid) {
  resetView();
  sessionId = sid;
  $("list").style.display = "none";
  $("review").style.display = "block";
  $("backBar").style.display = "block";
  $("reviewInfo").textContent = "ペーパーを読み込み中…";
  $("fixedPaper").style.display = "none";
  $("blocks").innerHTML = '<div class="muted">📝 下書きを読み込んでいます…</div>';
  $("rvApprove").disabled = true; $("rvConfirm").checked = false;
  pollPaper();
}
function resetView() {
  if (timer) clearInterval(timer);
  sessionId = null; kind = null; since = 0; lastPaperMd = null; paperPollMisses = 0; paperReadyNotifiedFor = null;
  $("log").innerHTML = ""; $("paperBody").innerHTML = ""; $("paperLive").textContent = ""; $("conclusionBody").textContent = "";
  $("live").style.display = "none"; $("review").style.display = "none";
  $("conclusion").style.display = "none"; $("say").style.display = "none";
  $("backBar").style.display = "none"; $("start").style.display = "none";
  $("fixedPaper").style.display = "none"; $("understanding").style.display = "none";
  $("debatability").style.display = "none";
  $("voiceSimulation").style.display = "none"; $("fixSuggestions").style.display = "none"; $("mechanicsKnowledge").style.display = "none"; $("reviewLearning").style.display = "none";
  $("go").disabled = false;
}
// 既存議論を開いて閲覧する (進行中はライブ、収束済みは最終状態)。
function openSession(sid) {
  resetView();
  sessionId = sid;
  $("list").style.display = "none";
  startLive();
}
$("sessionList").addEventListener("click", (e) => {
  const delBtn = e.target.closest(".sess-del");
  if (delBtn) {
    const th = delBtn.previousElementSibling?.querySelector(".th")?.textContent;
    deleteSession(delBtn.dataset.del, th);
    return;
  }
  const btn = e.target.closest(".sess"); if (!btn) return;
  if (btn.dataset.state === "draft") openDraft(btn.dataset.sid);
  else openSession(btn.dataset.sid);
});
$("filters").addEventListener("click", (e) => {
  const btn = e.target.closest(".filt"); if (!btn) return;
  curState = btn.dataset.state;
  for (const f of document.querySelectorAll("#filters .filt")) f.classList.toggle("active", f === btn);
  loadSessions(true);
});
$("scopeFilters").addEventListener("click", (e) => {
  const btn = e.target.closest(".scopeFilt"); if (!btn) return;
  curScope = btn.dataset.scope;
  for (const f of document.querySelectorAll("#scopeFilters .scopeFilt")) f.classList.toggle("active", f === btn);
  loadSessions(true);
});
$("loadMore").addEventListener("click", () => loadSessions(false));
$("newDiscussion").addEventListener("click", () => {
  $("list").style.display = "none"; $("start").style.display = "block";
});
$("backToList").addEventListener("click", () => {
  resetView();
  $("list").style.display = "block";
  loadSessions();
});

let lastPaperMd = null;
function stanceClass(stance, role) {
  if (role === "user") return "user";
  if (stance === "pro") return "pro";
  if (stance === "con") return "con";
  if (stance === "opinion") return "opinion";
  return "neutral";
}
function stanceText(stance, role) {
  if (role === "user") return "人間";
  if (stance === "pro") return "賛成派";
  if (stance === "con") return "反対派";
  if (stance === "opinion") return "中立";
  if (stance === "neutral") return "中立";
  return stance || "中立";
}
function utteranceMarks(u) {
  const marks = ['<span class="badge">' + escapeHtml(stanceText(u.stance, u.role)) + '</span>'];
  if (u.votes > 0) marks.push('<span class="mark vote">投票 ' + u.votes + '</span>');
  if (u.isWinner) marks.push('<span class="mark win">採択</span>');
  if (u.roundAufhebung && u.roundAufhebung.length > 0) marks.push('<span class="mark auf">止揚 ' + u.roundAufhebung.length + '</span>');
  if (u.isError) marks.push('<span class="mark">エラー</span>');
  return marks.join("");
}
function findUtteranceNode(id) {
  return [...document.querySelectorAll(".u[data-id]")].find((el) => el.dataset.id === id) || null;
}
function updateUtteranceMarks(node, u) {
  const el = node.querySelector(".marks");
  if (el) el.innerHTML = utteranceMarks(u);
}
async function poll() {
  if (!sessionId) return;
  const res = await fetch("/api/flow/" + sessionId + "/status?since=" + since).then(r => r.json()).catch(() => null);
  if (!res || !res.ok) return;
  // ディスカッションペーパー (議論進行で更新されていく) を描画。変化したらハイライト。
  if (res.paperMd != null) {
    $("paperPanel").style.display = "block";
    if (res.paperMd !== lastPaperMd) {
      $("paperBody").innerHTML = renderPaperMd(res.paperMd);
      if (lastPaperMd !== null) {
        $("paperLive").textContent = "更新 " + new Date().toLocaleTimeString();
        $("paperBody").classList.remove("upd"); void $("paperBody").offsetWidth; $("paperBody").classList.add("upd");
      }
      lastPaperMd = res.paperMd;
    }
  } else {
    $("paperPanel").style.display = "none";
  }
  for (const u of res.utterances) {
    since = Math.max(since, u.createdAt);
    const existing = findUtteranceNode(u.id);
    if (existing) { updateUtteranceMarks(existing, u); continue; }
    const div = document.createElement("div");
    div.dataset.id = u.id;
    div.className = "u " + stanceClass(u.stance, u.role) + (u.isError ? " err" : "");
    div.innerHTML = '<div class="who"><span>' + escapeHtml(u.displayName || u.personaName) + '</span><span class="marks">' + utteranceMarks(u) + '</span></div><div class="text">' + escapeHtml(u.text) + '</div>';
    $("log").appendChild(div);
  }
  if (Array.isArray(res.marks)) {
    for (const m of res.marks) {
      const existing = findUtteranceNode(m.id);
      if (existing) updateUtteranceMarks(existing, m);
    }
  }
  if (res.conclusion) {
    $("conclusion").style.display = "block";
    $("conclusionBody").textContent = res.conclusion;
  }
  if (res.done) { clearInterval(timer); $("go").disabled = false; $("paperLive").textContent = "確定"; }
}

// 軽量 markdown レンダラ (見出し/箇条書き/太字/段落のみ・XSS 安全に escape 後変換)。
function renderPaperMd(md) {
  const lines = String(md).split(/\\n/);
  let html = "", inList = false;
  const inline = (s) => escapeHtml(s).replace(/\\*\\*(.+?)\\*\\*/g, "<strong>$1</strong>");
  const closeList = () => { if (inList) { html += "</ul>"; inList = false; } };
  for (const raw of lines) {
    const line = raw.replace(/\\s+$/g, "");
    if (line === "") { closeList(); continue; }
    let m;
    if ((m = /^#\\s+(.+)$/.exec(line))) { closeList(); html += "<h3>" + inline(m[1]) + "</h3>"; }
    else if ((m = /^##\\s+(.+)$/.exec(line))) { closeList(); html += "<h4>" + inline(m[1]) + "</h4>"; }
    else if ((m = /^[-*]\\s+(.+)$/.exec(line))) { if (!inList) { html += "<ul>"; inList = true; } html += "<li>" + inline(m[1]) + "</li>"; }
    else { closeList(); html += "<p>" + inline(line) + "</p>"; }
  }
  closeList();
  return html;
}

function escapeHtml(s) { return String(s).replace(/[&<>]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c])); }

// 初期表示: 「すべて」フィルタをアクティブにして議論一覧を読み込む。
document.querySelector('#filters .filt[data-state="all"]').classList.add("active");
document.querySelector('#scopeFilters .scopeFilt[data-scope="' + curScope + '"]').classList.add("active");
$("paperToggle").addEventListener("click", () => {
  const panel = $("paperPanel");
  panel.classList.toggle("collapsed");
  $("paperToggle").textContent = panel.classList.contains("collapsed") ? "開く" : "閉じる";
});
$("githubCheck").addEventListener("click", () => { void checkGithubRead(); });
void loadGithubAuth();
loadSessions(true);
</script>
</body>
</html>`;
