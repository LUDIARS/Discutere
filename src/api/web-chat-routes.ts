/**
 * 軽量 Web チャット UI — Discord を介さず議論に参加する経路。
 *
 * Di の議論は Discord Gateway / フォーラムが正路だが、 ここでは同じ議論エンジン
 * (分類器 → designGap → persona-engine → facilitator) を `scene=web:<roomId>` という
 * トランスポートでそのまま再利用する。 Discord と異なり出力は外部投稿せず、 core に
 * 永続した発話をブラウザがポーリングで取得する。
 *
 *   GET  /chat                         チャット UI (1 ページ、 依存ゼロ、 loopback 前提)
 *   GET  /api/chat/:room/messages      会話取得 (?since=<ms> で増分)
 *   POST /api/chat/:room/messages      人間発話を投入 (進行中議論が無ければ designGap を起こす)
 *
 * 認証は他の HTTP UI と同じく loopback 信頼 (middleware/auth)。 個人データは保存しない
 * (author は表示名のみ)。
 */

import { Hono } from "hono";

import { getConfig } from "../config.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { submitMessage } from "../core/projection/message-input.js";
import type { DiscordAutoDiscussionInput } from "../discord-hook/auto-discussion.js";
import { listFlowSessions, type FlowSessionState } from "../flow/discussion-paper.js";
import { ensureWebSession, sanitizeRoomId, webDiscussionExists } from "../web-chat/session.js";
import { readRoomTranscript, type SpeakerNameResolver } from "../web-chat/transcript.js";

export interface WebChatDeps {
  workspaceId: string;
  /**
   * 平文投稿から議題を自動検出して persona-engine の議論開始へつなぐ
   * (gateway と同じ `createDiscordAutoDiscussionStarter` を注入)。
   */
  classifyInboundMessage?: (
    input: DiscordAutoDiscussionInput
  ) => Promise<{ started: boolean }> | Promise<void> | void;
  /** persona id → 表示名 (peDb の personas から解決)。 */
  resolveSpeakerName?: SpeakerNameResolver;
}

let deps: WebChatDeps = { workspaceId: "knowledge" };

export function setWebChatDeps(d: WebChatDeps): void {
  deps = d;
}

export const webChatRoutes = new Hono();

// ─── チャット UI (静的 HTML) ─────────────────────────────────
webChatRoutes.get("/chat", (c) => c.html(CHAT_HTML));

function parseChatSessionState(v: string | undefined): FlowSessionState {
  return v === "draft" || v === "live" || v === "concluded" ? v : "all";
}

function clampIntQuery(v: string | undefined, fallback: number, min: number, max: number): number {
  const n = Number.parseInt(v ?? "", 10);
  if (!Number.isFinite(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}

webChatRoutes.get("/api/chat/sessions", (c) => {
  const state = parseChatSessionState(c.req.query("state"));
  const limit = clampIntQuery(c.req.query("limit"), 50, 1, 100);
  const offset = clampIntQuery(c.req.query("offset"), 0, 0, Number.MAX_SAFE_INTEGER);
  const { rows, total } = listFlowSessions({ state, scope: "chat", limit, offset });
  return c.json({
    ok: true,
    total,
    limit,
    offset,
    hasMore: offset + rows.length < total,
    sessions: rows.map((r) => {
      const concluded = r.concluded === 1;
      const st = r.status === "draft" ? "draft" : concluded ? "concluded" : "live";
      return { ...r, concluded, state: st };
    }),
  });
});

// ─── 会話取得 (増分ポーリング) ───────────────────────────────
webChatRoutes.get("/api/chat/:room/messages", (c) => {
  const room = sanitizeRoomId(c.req.param("room"));
  const since = Number.parseInt(c.req.query("since") ?? "0", 10) || 0;
  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    const messages = readRoomTranscript(core, deps.workspaceId, room, {
      sinceTs: since,
      resolveName: deps.resolveSpeakerName,
    });
    const discussing = webDiscussionExists(core, deps.workspaceId, room);
    return c.json({ ok: true, room, discussing, messages });
  } finally {
    core.close();
  }
});

// ─── 人間発話の投入 ──────────────────────────────────────────
webChatRoutes.post("/api/chat/:room/messages", async (c) => {
  const room = sanitizeRoomId(c.req.param("room"));
  const body = (await c.req.json().catch(() => ({}))) as { text?: unknown; author?: unknown };
  const text = typeof body.text === "string" ? body.text.trim() : "";
  const author =
    typeof body.author === "string" && body.author.trim()
      ? body.author.trim().slice(0, 40)
      : "参加者";
  if (!text) return c.json({ ok: false, error: "text required" }, 400);

  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    const sessionId = ensureWebSession(core, deps.workspaceId, room);
    const res = submitMessage({
      core,
      workspaceId: deps.workspaceId,
      sessionId,
      personId: author,
      rawContent: text,
    });

    // スラッシュ風コマンド (utteranceId 無し) はそのまま返す。
    if (!res.utteranceId) {
      return c.json({ ok: true, room, seeded: false, commandResult: res.commandResult });
    }

    // この room でまだ議論が立っていなければ分類器で designGap を起こす。
    // 立っていれば event-bridge が人間発話として進行中議論にミラーし即応させる。
    let seeded = false;
    if (deps.classifyInboundMessage && !webDiscussionExists(core, deps.workspaceId, room)) {
      const r = await Promise.resolve(
        deps.classifyInboundMessage({
          workspaceId: deps.workspaceId,
          guildId: "web",
          channelId: room,
          sessionId,
          utteranceId: res.utteranceId,
          authorId: author,
          content: text,
          // Web チャットは本文を主題アンカーにして確実に議題化する (ゲーム名未特定でも開始)。
          forumTitle: text.slice(0, 80),
        })
      ).catch((err) => {
        console.warn(`  web-chat: classify failed: ${(err as Error).message}`);
        return null;
      });
      seeded = !!(r && typeof r === "object" && (r as { started?: boolean }).started === true);
    }

    return c.json({ ok: true, room, utteranceId: res.utteranceId, seeded });
  } finally {
    core.close();
  }
});

const CHAT_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>Discutere — チャット議論</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--muted:#64748b;--surface:#fff;--soft:#eaf2ff;--field:#fff;--border:#d0d7de;--primary:#4a90e2;--primary-fg:#fff;--secondary:#e2e8f0;--secondary-fg:#334155;--success:#16a34a;--human-bg:#eff6ff;--human-border:#93c5fd;--persona-bg:#f5f3ff;--persona-border:#c4b5fd;--facilitator-bg:#ecfdf5;--facilitator-border:#86efac;--external-bg:#f8fafc;--external-border:#cbd5e1}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--muted:#8b94a3;--surface:#161922;--soft:#4a90e21a;--field:#0c0e13;--border:#334155;--primary:#4a90e2;--primary-fg:#fff;--secondary:#2a2f3a;--secondary-fg:#e5e7eb;--success:#22c55e;--human-bg:#102033;--human-border:#2563eb;--persona-bg:#211634;--persona-border:#7c3aed;--facilitator-bg:#12251b;--facilitator-border:#15803d;--external-bg:#111827;--external-border:#475569}}
 *{box-sizing:border-box}
 body{font-family:system-ui,-apple-system,"Segoe UI","Hiragino Kaku Gothic ProN",sans-serif;margin:24px auto;padding:0 16px 32px;background:var(--bg);color:var(--fg);max-width:1100px;line-height:1.5}
 .app-head{border-bottom:2px solid var(--primary);padding-bottom:10px;margin-bottom:16px;display:flex;align-items:flex-end;justify-content:space-between;gap:12px;flex-wrap:wrap}
 h1{font-size:22px;margin:0}
 .mode-title{display:inline-flex;align-items:center;padding:4px 12px;border-radius:999px;background:var(--soft);font-weight:700;font-size:13px}
 .toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:0 0 16px}
 .toolbar input{background:var(--field);border:1px solid var(--border);border-radius:7px;color:var(--fg);padding:8px 10px;font:inherit;min-width:120px}
 .toolbar a{color:var(--primary);text-decoration:none;font-weight:700}
 .panel{border:1px solid var(--border);border-radius:8px;padding:16px;margin:16px 0;background:var(--surface)}
 .panel-head{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:10px;flex-wrap:wrap}
 .panel h2{font-size:16px;margin:0}
 .muted{color:var(--muted);font-size:13px}
 .session-list{display:grid;gap:8px}
 .sess{border:1px solid var(--border);border-radius:8px;padding:9px 11px;background:var(--surface);font:inherit;color:var(--fg);text-align:left}
 .sess .th{font-weight:700}
 .sess .meta{font-size:12px;color:var(--muted);margin-top:2px}
 #stateBox{border-left:4px solid var(--success);font-weight:700}
 #live{display:flex;flex-direction:column;gap:16px}
 #paperBody{white-space:pre-wrap;word-break:break-word;color:var(--fg)}
 #paperBody .topic{font-weight:700}
 #paperToggle{display:none;background:var(--secondary);color:var(--secondary-fg);border:1px solid var(--border);border-radius:6px;padding:5px 10px;font:inherit;font-size:12px;cursor:pointer}
 #log{display:grid;gap:10px}
 .msg{padding:12px 14px;border:1px solid var(--external-border);border-left-width:4px;border-radius:8px;background:var(--external-bg);white-space:pre-wrap;word-break:break-word;font-size:14px}
 .msg .who{font-weight:700;font-size:13px;margin-bottom:6px;display:flex;gap:8px;align-items:center;flex-wrap:wrap}
 .msg .tag{display:inline-block;font-size:11px;border-radius:5px;padding:2px 7px;background:var(--soft);font-weight:700}
 .human{background:var(--human-bg);border-color:var(--human-border)}
 .persona{background:var(--persona-bg);border-color:var(--persona-border)}
 .facilitator{background:var(--facilitator-bg);border-color:var(--facilitator-border)}
 .external{background:var(--external-bg);border-style:dashed}
 .empty{color:var(--muted);text-align:center;padding:28px 12px;font-size:13px;line-height:1.7}
 form{display:flex;gap:8px;align-items:stretch}
 form textarea{flex:1;resize:none;background:var(--field);border:1px solid var(--border);border-radius:7px;color:var(--fg);padding:10px 12px;font:inherit;max-height:160px}
 form button{background:var(--primary);border:1px solid var(--primary);border-radius:6px;color:var(--primary-fg);padding:0 18px;font:inherit;font-weight:700;cursor:pointer}
 form button:disabled{opacity:.55;cursor:default}
 @media (max-width:720px){body{margin:14px auto;padding:0 12px 28px}.panel{padding:12px}.toolbar input{width:100%;min-width:0}#paperToggle{display:inline-block}#paperPanel.collapsed #paperBody{display:none}form{flex-direction:column}form button{min-height:42px}}
</style></head><body>
<header class="app-head">
 <h1>チャット議論</h1>
 <div class="mode-title">人間とAI</div>
</header>
<div class="toolbar">
 <span id="status" class="muted">接続中…</span>
 <input id="room" title="議論ルーム" value="lobby" size="10">
 <input id="author" title="表示名" placeholder="あなたの名前" value="" size="10">
 <a href="/" title="トップへ">↩</a>
</div>
<div id="sessionPanel" class="panel">
 <div class="panel-head"><h2>チャット議論一覧</h2><span class="muted">壁打ちのみ</span></div>
 <div id="sessionList" class="session-list"><div class="muted">読み込み中…</div></div>
</div>
<div id="stateBox" class="panel">収束結果<br><span class="muted" id="stateText">議論の開始を待っています。</span></div>
<div id="live">
 <div id="paperPanel" class="panel">
  <div class="panel-head"><h2>ディスカッションペーパー</h2><button id="paperToggle" type="button">閉じる</button></div>
  <div id="paperBody"><span class="topic">議題</span><br>最初の投稿を議題として、人間とAIの議論を開始します。</div>
 </div>
 <div id="opinions" class="panel">
  <div class="panel-head"><h2>各LLMの意見議論内容</h2><span class="muted" id="count">0 件</span></div>
  <div id="log"><div class="empty">話題を投げると AI の論者たちが議論を始めます。<br>例:「ローグライト系の面白さの核は何か」</div></div>
 </div>
</div>
<form id="f" class="panel">
 <textarea id="t" rows="1" placeholder="メッセージを入力 (Enter で送信 / Shift+Enter で改行)"></textarea>
 <button id="send" type="submit">送信</button>
</form>
<script>
const log=document.getElementById('log'),statusEl=document.getElementById('status');
const stateText=document.getElementById('stateText'),countEl=document.getElementById('count'),paperBody=document.getElementById('paperBody');
const roomEl=document.getElementById('room'),authorEl=document.getElementById('author');
const ta=document.getElementById('t'),form=document.getElementById('f'),sendBtn=document.getElementById('send');
const sessionList=document.getElementById('sessionList');
let since=0,seen=new Set(),room=roomEl.value,first=true,messageCount=0,topic='';
authorEl.value=localStorage.getItem('di-author')||'';
authorEl.addEventListener('change',()=>localStorage.setItem('di-author',authorEl.value.trim()));

function esc(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
function roleLabel(role){return role==='human'?'人間':role==='persona'?'AI論者':role==='facilitator'?'進行役':'外部の声';}
function stateLabel(s){return s==='draft'?'下書き':s==='concluded'?'収束済み':'進行中';}
function updateTopic(m){
 if(topic||m.role!=='human')return;
 topic=m.content.slice(0,160);
 paperBody.innerHTML='<span class="topic">議題</span><br>'+esc(topic);
}
function render(m){
 if(seen.has(m.id))return; seen.add(m.id);
 if(first){log.innerHTML='';first=false;}
 updateTopic(m);
 const d=document.createElement('div'); d.className='msg '+m.role;
 d.innerHTML='<div class="who"><span>'+esc(m.speaker)+'</span><span class="tag">'+roleLabel(m.role)+'</span></div>'+esc(m.content);
 log.appendChild(d); log.scrollTop=log.scrollHeight;
 messageCount++; countEl.textContent=messageCount+' 件';
}
async function poll(){
 try{
  const r=await fetch('/api/chat/'+encodeURIComponent(room)+'/messages?since='+since);
  const j=await r.json();
  if(j.ok){
   for(const m of j.messages){ render(m); if(m.postedAt>since)since=m.postedAt; }
   statusEl.textContent=j.discussing?'● 議論中':'待機中';
   stateText.textContent=j.discussing?'議論が進行中です。AIの応答を待っています。':'議論の開始を待っています。';
  }
 }catch(e){ statusEl.textContent='接続エラー'; }
}
async function loadSessions(){
 try{
  const r=await fetch('/api/chat/sessions?limit=20&offset=0');
  const j=await r.json();
  if(!j.ok)throw new Error('bad response');
  if(!j.sessions.length){sessionList.innerHTML='<div class="muted">チャット議論はまだありません。</div>';return;}
  sessionList.innerHTML='';
  for(const s of j.sessions){
   const row=document.createElement('div');
   row.className='sess';
   row.innerHTML='<div class="th">'+esc(s.title||s.theme||'(無題)')+'</div><div class="meta">'+stateLabel(s.state)+' / '+esc(s.discussionType||s.flow)+' / 発話 '+s.utterances+' 件</div>';
   sessionList.appendChild(row);
  }
 }catch(e){sessionList.innerHTML='<div class="muted">一覧の取得に失敗しました。</div>';}
}
function switchRoom(){
 const nv=(roomEl.value||'lobby').trim();
 if(nv===room)return;
 room=nv; since=0; seen=new Set(); first=true; messageCount=0; topic='';
 countEl.textContent='0 件';
 paperBody.innerHTML='<span class="topic">議題</span><br>最初の投稿を議題として、人間とAIの議論を開始します。';
 log.innerHTML='<div class="empty">ルーム「'+esc(room)+'」</div>'; poll();
}
roomEl.addEventListener('change',switchRoom);
async function send(){
 const text=ta.value.trim(); if(!text)return;
 sendBtn.disabled=true;
 try{
  await fetch('/api/chat/'+encodeURIComponent(room)+'/messages',{
   method:'POST',headers:{'Content-Type':'application/json'},
   body:JSON.stringify({text,author:authorEl.value.trim()})
  });
  ta.value=''; ta.style.height='auto'; await poll();
 }catch(e){ statusEl.textContent='送信失敗'; }
 finally{ sendBtn.disabled=false; ta.focus(); }
}
form.addEventListener('submit',e=>{e.preventDefault();send();});
ta.addEventListener('keydown',e=>{ if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send();} });
ta.addEventListener('input',()=>{ta.style.height='auto';ta.style.height=Math.min(ta.scrollHeight,140)+'px';});
document.getElementById('paperToggle').addEventListener('click',()=>{
 const panel=document.getElementById('paperPanel');
 panel.classList.toggle('collapsed');
 document.getElementById('paperToggle').textContent=panel.classList.contains('collapsed')?'開く':'閉じる';
});
poll(); loadSessions(); setInterval(poll,2500);
</script></body></html>`;
