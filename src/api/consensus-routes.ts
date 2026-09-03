/**
 * 合意スコア可視化 (FEATURE ③+④)。
 *
 * - GET /admin/consensus           → ライブ管制ページ (gap ドロップダウン + 意見ごとの 👍/agree/score)
 * - GET /admin/consensus/gaps      → 直近の議論 gap 一覧 (ドロップダウンのソース)
 * - GET /admin/consensus/:gapId    → その議論の意見 + consensus_scores (人間は「人間」タグ)
 *
 * 人間 / AI の意見を同一レイヤで並べる (区別せず score 順)。admin role 必須。
 * Discatier Core は read-only で都度 open/close (queue-routes と同じ短命接続)。
 */

import { Hono } from "hono";
import type { Context } from "hono";

import { getUserId, getUserRole } from "../middleware/auth.js";
import { createCore } from "../core/index.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { getConfig } from "../config.js";
import { FACILITATOR_PERSONA_ID } from "../persona-engine/facilitator/facilitator.js";
import { consensusPersonaRoutes } from "./consensus-persona-routes.js";
import { gapCreationRoutes } from "./gap-creation-routes.js";

export const consensusRoutes = new Hono();

consensusRoutes.route("/", consensusPersonaRoutes);
consensusRoutes.route("/", gapCreationRoutes);

interface ConsensusGap {
  gapId: string;
  sessionId: string;
  title: string;
  status: string | null;
  scoredCount: number;
}

interface ConsensusOpinion {
  utteranceId: string;
  speaker: string;
  human: boolean;
  content: string;
  agree: boolean | null;
  score: number | null;
  reasoning: string | null;
}

consensusRoutes.get("/admin/consensus", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  return c.html(HTML);
});

consensusRoutes.get("/admin/consensus/gaps", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const ws = getConfig().workspace;
  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    const rows = core.client.raw
      .prepare(
        `SELECT g.id AS gapId, g.title AS title, g.status AS status, g.updated_at AS updatedAt, s.id AS sessionId
           FROM design_gaps g
           JOIN sessions s
             ON s.workspace_id = g.workspace_id
            AND s.title = 'discussion-of-gap:' || g.id
          WHERE g.workspace_id = ?
          ORDER BY g.updated_at DESC
          LIMIT 50`
      )
      .all(ws) as Array<{ gapId: string; title: string; status: string | null; sessionId: string }>;
    const gaps: ConsensusGap[] = rows.map((r) => ({
      gapId: r.gapId,
      sessionId: r.sessionId,
      title: r.title,
      status: r.status,
      scoredCount: scoredCount(core, r.sessionId),
    }));
    return c.json({ gaps });
  } finally {
    core.close();
  }
});

consensusRoutes.get("/admin/consensus/:gapId", (c) => {
  const guard = requireAdmin(c);
  if (guard) return guard;
  const ws = getConfig().workspace;
  const gapId = c.req.param("gapId");
  const core = createCore(resolveActiveKgPath(getConfig()));
  try {
    const session = core.client.raw
      .prepare(
        "SELECT id FROM sessions WHERE workspace_id = ? AND title = ? ORDER BY started_at DESC LIMIT 1"
      )
      .get(ws, `discussion-of-gap:${gapId}`) as { id: string } | undefined;
    if (!session) return c.json({ gapId, opinions: [] });
    return c.json({ gapId, opinions: opinionsFor(core, session.id) });
  } finally {
    core.close();
  }
});

function scoredCount(core: ReturnType<typeof createCore>, sessionId: string): number {
  const row = core.client.raw
    .prepare(
      `SELECT COUNT(*) AS n FROM consensus_scores
        WHERE utterance_id IN (SELECT id FROM utterances WHERE session_id = ?)`
    )
    .get(sessionId) as { n: number } | undefined;
  return row?.n ?? 0;
}

function opinionsFor(core: ReturnType<typeof createCore>, sessionId: string): ConsensusOpinion[] {
  const raw = core.client.raw;
  const rows = raw
    .prepare(
      `SELECT u.id AS id, u.speaker_id AS speakerId, u.raw_content AS content,
              cs.agree AS agree, cs.score AS score, cs.reasoning AS reasoning
         FROM utterances u
         LEFT JOIN consensus_scores cs ON cs.utterance_id = u.id
        WHERE u.session_id = ?
          AND u.speaker_id IS NOT NULL
          AND u.speaker_id NOT LIKE 'ext:%'
          AND u.speaker_id <> ?
        ORDER BY COALESCE(cs.score, -1) DESC, u.posted_at ASC`
    )
    .all(sessionId, `persona:${FACILITATOR_PERSONA_ID}`) as Array<{
    id: string;
    speakerId: string;
    content: string;
    agree: number | null;
    score: number | null;
    reasoning: string | null;
  }>;
  return rows
    .filter((r) => !r.content.startsWith("【収束】"))
    .map((r) => {
      const human = isHumanSpeaker(r.speakerId);
      return {
        utteranceId: r.id,
        speaker: speakerLabel(r.speakerId),
        human,
        content: r.content,
        agree: r.agree == null ? null : r.agree === 1,
        score: r.score,
        reasoning: r.reasoning,
      };
    });
}

function isHumanSpeaker(speakerId: string | null): boolean {
  return !!speakerId && !speakerId.startsWith("persona:") && !speakerId.startsWith("ext:");
}

function speakerLabel(speakerId: string): string {
  // personas テーブルは persona-engine DB 側にあり KG (core) には無いので、
  // conclusions.ts と同じく persona id をそのままラベルにする (人間は「人間」)。
  if (speakerId.startsWith("persona:")) return speakerId.slice("persona:".length);
  return "人間";
}

function requireAdmin(c: Context): Response | null {
  const userId = getUserId(c);
  const role = getUserRole(c);
  if (!userId) return c.json({ error: "Authentication required" }, 401);
  if (role !== "admin") return c.json({ error: "admin role required" }, 403);
  return null;
}

const HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>Discutere — 合意スコア</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--muted:#64748b;--surface:#fff;--border:#e2e8f0;--link:#2563eb;--tag-human-bg:#ede9fe;--tag-human-fg:#6d28d9;--tag-agree-bg:#dcfce7;--tag-agree-fg:#15803d;--tag-disagree-bg:#fee2e2;--tag-disagree-fg:#b91c1c}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--muted:#8b94a3;--surface:#161922;--border:#232733;--link:#60a5fa;--tag-human-bg:#7c3aed33;--tag-human-fg:#c4b5fd;--tag-agree-bg:#16a34a33;--tag-agree-fg:#86efac;--tag-disagree-bg:#dc262633;--tag-disagree-fg:#fca5a5}}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:var(--fg)}
 .wrap{max-width:900px;margin:0 auto;padding:32px 24px}
 h1{font-size:20px;margin:0 0 4px} .sub{color:var(--muted);font-size:13px;margin:0 0 20px}
 select{background:var(--surface);color:var(--fg);border:1px solid var(--border);border-radius:8px;padding:8px 12px;font-size:14px;width:100%;max-width:560px}
 table{width:100%;border-collapse:collapse;font-size:13px;margin-top:16px}
 th,td{padding:8px 10px;text-align:left;border-bottom:1px solid var(--border);vertical-align:top}
 th{background:var(--surface);color:var(--muted)}
 .tag{display:inline-block;border-radius:6px;padding:1px 7px;font-size:11px;margin-left:6px}
 .tag.human{background:var(--tag-human-bg);color:var(--tag-human-fg)}
 .tag.agree{background:var(--tag-agree-bg);color:var(--tag-agree-fg)}
 .tag.disagree{background:var(--tag-disagree-bg);color:var(--tag-disagree-fg)}
 .score{font-weight:700} .muted{color:var(--muted)}
 a{color:var(--link)}
</style></head><body><div class="wrap">
 <h1>合意スコア <span class="muted" id="refresh"></span></h1>
 <p class="sub">AI が各意見 (人間も AI も同一評価) の合意度を採点。 👍=議論の総意が同意。 <a href="/">← トップ</a></p>
 <select id="gap"><option>読み込み中…</option></select>
 <table id="opinions"><thead><tr><th>発言者</th><th>意見</th><th>👍</th><th>score</th></tr></thead>
  <tbody><tr><td colspan="4" class="muted">gap を選択</td></tr></tbody></table>
<script>
function esc(s){return String(s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#039;"}[c]));}
async function loadGaps(){
  const sel=document.getElementById('gap');
  const cur=sel.value;
  try{
    const r=await (await fetch('/api/admin/consensus/gaps',{credentials:'include'})).json();
    if(!r.gaps||!r.gaps.length){sel.innerHTML='<option value="">(議論なし)</option>';return;}
    sel.innerHTML=r.gaps.map(g=>'<option value="'+esc(g.gapId)+'">'+esc(g.title)+' ['+esc(g.status||'open')+'] 採点'+g.scoredCount+'</option>').join('');
    if(cur)sel.value=cur;
    if(!sel.value)sel.selectedIndex=0;
    loadOpinions();
  }catch(e){sel.innerHTML='<option value="">'+esc(e.message)+'</option>';}
}
async function loadOpinions(){
  const gapId=document.getElementById('gap').value;
  const tb=document.querySelector('#opinions tbody');
  if(!gapId){tb.innerHTML='<tr><td colspan="4" class="muted">gap を選択</td></tr>';return;}
  try{
    const r=await (await fetch('/api/admin/consensus/'+encodeURIComponent(gapId),{credentials:'include'})).json();
    if(!r.opinions||!r.opinions.length){tb.innerHTML='<tr><td colspan="4" class="muted">意見なし</td></tr>';return;}
    tb.innerHTML=r.opinions.map(o=>{
      const human=o.human?'<span class="tag human">人間</span>':'';
      const agree=o.agree===null?'<span class="muted">未採点</span>':(o.agree?'<span class="tag agree">👍 同意</span>':'<span class="tag disagree">非同意</span>');
      const score=o.score===null?'<span class="muted">—</span>':'<span class="score">'+o.score+'</span>';
      const reason=o.reasoning?'<div class="muted">'+esc(o.reasoning)+'</div>':'';
      return '<tr><td>'+esc(o.speaker)+human+'</td><td>'+esc(o.content)+reason+'</td><td>'+agree+'</td><td>'+score+'</td></tr>';
    }).join('');
  }catch(e){tb.innerHTML='<tr><td colspan="4" class="muted">'+esc(e.message)+'</td></tr>';}
  document.getElementById('refresh').textContent='更新 '+new Date().toLocaleTimeString();
}
document.getElementById('gap').addEventListener('change',loadOpinions);
loadGaps();
setInterval(loadGaps,5000);
</script></div></body></html>`;
