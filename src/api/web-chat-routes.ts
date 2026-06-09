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
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Discutere — チャット議論</title>
<style>
 :root{--bg:#0f1115;--panel:#161922;--border:#232733;--accent:#2563eb;--muted:#8b94a3}
 *{box-sizing:border-box}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:#e6e6e6;height:100dvh;display:flex;flex-direction:column}
 header{padding:10px 16px;border-bottom:1px solid var(--border);display:flex;gap:10px;align-items:center;flex-wrap:wrap}
 header h1{font-size:16px;margin:0;font-weight:600}
 header .sp{flex:1}
 header input{background:var(--panel);border:1px solid var(--border);border-radius:10px;color:#e6e6e6;padding:7px 12px;font-size:13px}
 #status{font-size:12px;color:var(--muted)}
 #log{flex:1;overflow-y:auto;padding:16px;display:flex;flex-direction:column;gap:10px}
 .msg{max-width:78%;padding:9px 13px;border-radius:14px;line-height:1.55;white-space:pre-wrap;word-break:break-word;font-size:14px}
 .msg .who{font-size:11px;color:var(--muted);margin-bottom:3px}
 .human{align-self:flex-end;background:var(--accent);color:#fff;border-bottom-right-radius:4px}
 .human .who{color:#cfe0ff}
 .persona{align-self:flex-start;background:var(--panel);border:1px solid var(--border);border-bottom-left-radius:4px}
 .facilitator{align-self:center;background:#1d2331;border:1px solid #2d3650;color:#cdd6f4;font-size:13px;max-width:90%}
 .external{align-self:flex-start;background:#1a1f2b;border:1px dashed #34405a;color:#b9c2d4}
 .empty{color:var(--muted);text-align:center;margin-top:40px;font-size:13px;line-height:1.7}
 form{display:flex;gap:8px;padding:12px 16px;border-top:1px solid var(--border)}
 form textarea{flex:1;resize:none;background:var(--panel);border:1px solid var(--border);border-radius:12px;color:#e6e6e6;padding:11px 14px;font-size:14px;font-family:inherit;max-height:140px}
 form button{background:var(--accent);border:0;border-radius:12px;color:#fff;padding:0 20px;font-size:14px;font-weight:600;cursor:pointer}
 form button:disabled{opacity:.5;cursor:default}
 a{color:#6ea8ff}
</style></head><body>
<header>
 <h1>Discutere チャット</h1>
 <span id="status">接続中…</span>
 <span class="sp"></span>
 <input id="room" title="議論ルーム" value="lobby" size="10">
 <input id="author" title="表示名" placeholder="あなたの名前" value="" size="10">
 <a href="/" title="トップへ">↩</a>
</header>
<div id="log"><div class="empty">話題を投げると AI の論者たちが議論を始めます。<br>例:「ローグライト系の面白さの核は何か」</div></div>
<form id="f">
 <textarea id="t" rows="1" placeholder="メッセージを入力 (Enter で送信 / Shift+Enter で改行)"></textarea>
 <button id="send" type="submit">送信</button>
</form>
<script>
const log=document.getElementById('log'),statusEl=document.getElementById('status');
const roomEl=document.getElementById('room'),authorEl=document.getElementById('author');
const ta=document.getElementById('t'),form=document.getElementById('f'),sendBtn=document.getElementById('send');
let since=0,seen=new Set(),room=roomEl.value,first=true;
authorEl.value=localStorage.getItem('di-author')||'';
authorEl.addEventListener('change',()=>localStorage.setItem('di-author',authorEl.value.trim()));

function esc(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
function render(m){
 if(seen.has(m.id))return; seen.add(m.id);
 if(first){log.innerHTML='';first=false;}
 const d=document.createElement('div'); d.className='msg '+m.role;
 d.innerHTML='<div class="who">'+esc(m.speaker)+'</div>'+esc(m.content);
 log.appendChild(d); log.scrollTop=log.scrollHeight;
}
async function poll(){
 try{
  const r=await fetch('/api/chat/'+encodeURIComponent(room)+'/messages?since='+since);
  const j=await r.json();
  if(j.ok){
   for(const m of j.messages){ render(m); if(m.postedAt>since)since=m.postedAt; }
   statusEl.textContent=j.discussing?'● 議論中':'待機中';
  }
 }catch(e){ statusEl.textContent='接続エラー'; }
}
function switchRoom(){
 const nv=(roomEl.value||'lobby').trim();
 if(nv===room)return;
 room=nv; since=0; seen=new Set(); first=true;
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
poll(); setInterval(poll,2500);
</script></body></html>`;
