/**
 * ノイズ管理 API/UI (per-utterance soft-exclude, reversible)。
 *
 * 議論データ (結論 / データソース) から「別ゲームの雑談」「人間の煽り」等のノイズ発話を
 * 可逆的に除外する。除外は utterance_exclusions サイドカー表に積み (src/core/noise/exclusions.ts)、
 * 新まとめ生成時 (facilitator.converge) は即時に弾かれる。既存キャッシュ (build:learning-cache)
 * への反映は再生成後。
 *
 * Discatier Core は read/write で都度 open/close (queue-routes と同じ短命接続パターン)。
 * loopback-admin: admin role 必須。
 */

import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { getConfig } from "../config.js";
import {
  ensureExclusionTable,
  excludeUtterance,
  restoreUtterance,
  listExcludedIds,
} from "../core/noise/exclusions.js";

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}

/** persona:<id> / ext:<...> / 人間 を人間可読ラベルに解決する。 */
function speakerLabel(raw: ReturnType<typeof createCore>["client"]["raw"], speakerId: string | null): string {
  if (!speakerId) return "参加者";
  if (speakerId.startsWith("persona:")) {
    const id = speakerId.slice("persona:".length);
    const p = raw.prepare("SELECT display_name FROM personas WHERE id = ?").get(id) as
      | { display_name: string | null }
      | undefined;
    return p?.display_name ?? id;
  }
  if (speakerId.startsWith("ext:")) return "外部の声";
  return "人間";
}

export function createNoiseRoutes(): Hono {
  const noiseRoutes = new Hono();

  noiseRoutes.get("/admin/noise", (c) => {
    const guard = requireAdmin(c);
    if (guard) return guard;
    return c.html(NOISE_HTML);
  });

  // 議論 session 一覧 (gap title + 発話数) と ソース種別 (title prefix) を返す。
  noiseRoutes.get("/admin/noise/groups", (c) => {
    const guard = requireAdmin(c);
    if (guard) return guard;
    const ws = getConfig().workspace;
    const core = createCore(resolveActiveKgPath(getConfig()));
    try {
      const raw = core.client.raw;
      ensureExclusionTable(raw);
      const sessRows = raw
        .prepare(
          `SELECT s.id AS sessionId, s.title AS title,
                  (SELECT COUNT(*) FROM utterances u WHERE u.session_id = s.id) AS utteranceCount
             FROM sessions s
            WHERE s.workspace_id = ? AND s.title LIKE 'discussion-of-gap:%'
            ORDER BY utteranceCount DESC`
        )
        .all(ws) as Array<{ sessionId: string; title: string; utteranceCount: number }>;
      const sessions = sessRows.map((r) => {
        const gapId = r.title.slice("discussion-of-gap:".length);
        const g = raw.prepare("SELECT title FROM design_gaps WHERE id = ?").get(gapId) as
          | { title: string }
          | undefined;
        return { sessionId: r.sessionId, gapId, title: g?.title ?? r.title, utteranceCount: r.utteranceCount };
      });
      // ソース種別: title の `<source>:<slug>` の source 部分で粗くグルーピング。
      const sources = raw
        .prepare(
          `SELECT SUBSTR(title, 1, INSTR(title, ':') - 1) AS source, COUNT(*) AS count
             FROM sessions
            WHERE workspace_id = ? AND INSTR(title, ':') > 0
            GROUP BY source
            ORDER BY count DESC`
        )
        .all(ws) as Array<{ source: string; count: number }>;
      return c.json({ sessions, sources });
    } finally {
      core.close();
    }
  });

  // session または source の発話一覧 (除外フラグ付き)。
  noiseRoutes.get("/admin/noise/utterances", (c) => {
    const guard = requireAdmin(c);
    if (guard) return guard;
    const ws = getConfig().workspace;
    const sessionId = c.req.query("sessionId");
    const source = c.req.query("source");
    if (!sessionId && !source) return c.json({ error: "sessionId or source required" }, 400);
    const core = createCore(resolveActiveKgPath(getConfig()));
    try {
      const raw = core.client.raw;
      const excluded = listExcludedIds(raw);
      let rows: Array<{ id: string; speaker_id: string | null; raw_content: string; posted_at: number }>;
      if (sessionId) {
        rows = raw
          .prepare(
            "SELECT id, speaker_id, raw_content, posted_at FROM utterances WHERE session_id = ? ORDER BY posted_at ASC LIMIT 300"
          )
          .all(sessionId) as typeof rows;
      } else {
        // source: title が `<source>:` で始まる session 群の発話を集約。
        rows = raw
          .prepare(
            `SELECT u.id, u.speaker_id, u.raw_content, u.posted_at
               FROM utterances u
               JOIN sessions s ON s.id = u.session_id
              WHERE s.workspace_id = ? AND s.title LIKE ?
              ORDER BY u.posted_at ASC LIMIT 300`
          )
          .all(ws, `${source}:%`) as typeof rows;
      }
      const out = rows.map((u) => ({
        id: u.id,
        speaker: speakerLabel(raw, u.speaker_id),
        content: u.raw_content,
        posted_at: u.posted_at,
        excluded: excluded.has(u.id),
      }));
      return c.json(out);
    } finally {
      core.close();
    }
  });

  noiseRoutes.post("/admin/noise/exclude", async (c) => {
    const guard = requireAdmin(c);
    if (guard) return guard;
    const body = await c.req
      .json<{ utteranceId?: string; reason?: string }>()
      .catch(() => ({}) as { utteranceId?: string; reason?: string });
    if (!body.utteranceId) return c.json({ error: "utteranceId required" }, 400);
    const core = createCore(resolveActiveKgPath(getConfig()));
    try {
      excludeUtterance(core.client.raw, body.utteranceId, body.reason);
      return c.json({ ok: true });
    } finally {
      core.close();
    }
  });

  noiseRoutes.post("/admin/noise/restore", async (c) => {
    const guard = requireAdmin(c);
    if (guard) return guard;
    const body = await c.req
      .json<{ utteranceId?: string }>()
      .catch(() => ({}) as { utteranceId?: string });
    if (!body.utteranceId) return c.json({ error: "utteranceId required" }, 400);
    const core = createCore(resolveActiveKgPath(getConfig()));
    try {
      restoreUtterance(core.client.raw, body.utteranceId);
      return c.json({ ok: true });
    } finally {
      core.close();
    }
  });

  return noiseRoutes;
}

const NOISE_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ノイズ管理 — Discutere</title>
<style>
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:#0f1115;color:#e6e6e6}
 .wrap{max-width:900px;margin:0 auto;padding:32px 24px}
 h1{font-size:20px;margin:0 0 2px} .sub{color:#8b94a3;font-size:13px;margin:0 0 22px}
 .row{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin-bottom:16px}
 label{font-size:13px;color:#8b94a3}
 select{background:#161922;color:#e6e6e6;border:1px solid #232733;border-radius:8px;padding:8px 10px;font-size:13px;min-width:240px}
 .note{font-size:12px;color:#a0863c;background:#1c1a12;border:1px solid #34301c;border-radius:8px;padding:8px 12px;margin-bottom:14px}
 .item{display:flex;gap:10px;align-items:flex-start;background:#161922;border:1px solid #232733;border-radius:10px;padding:10px 12px;margin-bottom:8px}
 .item.excluded{opacity:.5}
 .sp{font-size:12px;color:#7aa2f7;font-weight:600;white-space:nowrap;min-width:80px}
 .ct{flex:1;font-size:13px;line-height:1.5;word-break:break-word}
 .item.excluded .ct{text-decoration:line-through}
 button{background:#2563eb;color:#fff;border:none;border-radius:6px;padding:5px 12px;font-size:12px;cursor:pointer;white-space:nowrap}
 button.restore{background:#3a3f4b}
 a.back{color:#8b94a3;font-size:12px;text-decoration:none}
</style></head><body><div class="wrap">
 <a class="back" href="/">← トップへ</a>
 <h1>ノイズ管理</h1>
 <p class="sub">議論データ (結論 / データソース) から別ゲーム雑談・煽り等のノイズ発話を可逆的に除外</p>
 <div class="note" id="note">キャッシュ反映は build:learning-cache 再生成後。新まとめ生成時は即時除外。</div>
 <div class="row">
   <label>議論 session</label>
   <select id="sel-session"><option value="">— 選択 —</option></select>
   <label>ソース種別</label>
   <select id="sel-source"><option value="">— 選択 —</option></select>
 </div>
 <div id="list"><p class="sub">議論かソースを選択してください。</p></div>
</div>
<script>
function esc(s){return String(s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#039;"}[c]));}
async function loadGroups(){
  const r = await fetch("/api/admin/noise/groups",{credentials:"include"});
  if(!r.ok) return;
  const j = await r.json();
  const ss = document.getElementById("sel-session");
  j.sessions.forEach(s=>{ const o=document.createElement("option"); o.value="s:"+s.sessionId; o.textContent=s.title+" ("+s.utteranceCount+"発話)"; ss.appendChild(o); });
  const so = document.getElementById("sel-source");
  j.sources.forEach(s=>{ const o=document.createElement("option"); o.value="src:"+s.source; o.textContent=s.source+" ("+s.count+"session)"; so.appendChild(o); });
}
let current = null;
async function loadUtterances(){
  if(!current) return;
  const list = document.getElementById("list");
  list.innerHTML = '<p class="sub">読み込み中…</p>';
  const qs = current.kind==="session" ? "sessionId="+encodeURIComponent(current.value) : "source="+encodeURIComponent(current.value);
  const r = await fetch("/api/admin/noise/utterances?"+qs,{credentials:"include"});
  if(!r.ok){ list.innerHTML='<p class="sub">取得失敗: '+r.status+'</p>'; return; }
  const items = await r.json();
  if(!items.length){ list.innerHTML='<p class="sub">発話なし。</p>'; return; }
  list.innerHTML = items.map(u=>{
    const ct = u.content.length>240 ? esc(u.content.slice(0,240))+"…" : esc(u.content);
    const btn = u.excluded
      ? '<button class="restore" data-id="'+esc(u.id)+'" data-act="restore">戻す</button>'
      : '<button data-id="'+esc(u.id)+'" data-act="exclude">ノイズ除外</button>';
    return '<div class="item'+(u.excluded?' excluded':'')+'"><div class="sp">'+esc(u.speaker)+'</div><div class="ct">'+ct+'</div>'+btn+'</div>';
  }).join("");
  list.querySelectorAll("button").forEach(b=>b.addEventListener("click",()=>toggle(b.dataset.id,b.dataset.act)));
}
async function toggle(id,act){
  const ep = act==="exclude" ? "/api/admin/noise/exclude" : "/api/admin/noise/restore";
  await fetch(ep,{method:"POST",headers:{"Content-Type":"application/json"},credentials:"include",body:JSON.stringify({utteranceId:id})});
  loadUtterances();
}
document.getElementById("sel-session").addEventListener("change",e=>{
  if(!e.target.value){current=null;return;}
  document.getElementById("sel-source").value="";
  current={kind:"session",value:e.target.value.slice(2)}; loadUtterances();
});
document.getElementById("sel-source").addEventListener("change",e=>{
  if(!e.target.value){current=null;return;}
  document.getElementById("sel-session").value="";
  current={kind:"source",value:e.target.value.slice(4)}; loadUtterances();
});
loadGroups();
</script></body></html>`;
