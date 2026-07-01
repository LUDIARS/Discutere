/**
 * 議論設定 UI / API (port 3100)。
 *
 * facilitator の収束トリガールール (「20 意見で収束」= maxPersonas 等) と、
 * ワーカーの役割プロンプト / debate ルール instructions を、 再起動なしで編集する。
 * 値は runtime-settings ストア (SQLite) に override として保存され、 facilitator は
 * tick ごとに、 役割プロンプトはワーカー (再)起動時に、 debate instructions は再シードで
 * 反映される。
 *
 *   GET  /api/admin/tuning              編集 HTML
 *   GET  /api/admin/tuning/data         現在値 JSON
 *   PUT  /api/admin/tuning/facilitator  収束トリガー更新 (Partial<FacilitatorTuning>)
 *   PUT  /api/admin/tuning/role         役割プロンプト override {role, text|null}
 *   PUT  /api/admin/tuning/rule         debate instructions override {ruleId, text|null}
 *   PUT  /api/admin/tuning/cost-relay   コスト relay 設定 {enabled?, anatomiaUrl?, concordiaUrl?, service?}
 *
 * 認可は :3100 の他の操作 UI (worker-pool-control) と同じく loopback 運用前提で
 * guard を置かない (ブラウザ直開きでヘッダ注入不要)。 store は index.ts が
 * setRuntimeSettings() で注入する。
 */

import { Hono } from "hono";

import type { RuntimeSettingsStore } from "../runtime-settings/store.js";
import { readGcloudSecret } from "../secrets/gcloud-secret.js";
import {
  getInfisicalRuntimeSecret,
  getInfisicalRuntimeSecretStatus,
  setInfisicalRuntimeSecret,
} from "../secrets/infisical-runtime.js";

let store: RuntimeSettingsStore | null = null;

export function setRuntimeSettings(s: RuntimeSettingsStore | null): void {
  store = s;
}

/** debate ルール instructions の再シードを依頼するフック (index.ts が注入)。 */
let reseedRules: (() => void) | null = null;
export function setRuleReseeder(fn: (() => void) | null): void {
  reseedRules = fn;
}

const tuningRoutes = new Hono();
const YOUTUBE_API_KEY = "DISCUTERE_YOUTUBE_API_KEY";

tuningRoutes.get("/admin/tuning/data", (c) => {
  if (!store) return c.json({ enabled: false }, 503);
  return c.json({
    enabled: true,
    facilitator: store.getFacilitatorTuning(),
    rolePrompts: store.listRolePrompts(),
    ruleInstructions: store.listRuleInstructions(),
    costRelay: store.getCostRelay(),
    secrets: {
      youtubeApiKey: getInfisicalRuntimeSecretStatus(YOUTUBE_API_KEY),
    },
  });
});

tuningRoutes.post("/admin/tuning/secrets/youtube-api-key/refresh", async (c) => {
  try {
    const value = await getInfisicalRuntimeSecret(YOUTUBE_API_KEY, { refresh: true });
    return c.json({ ok: true, youtubeApiKey: { ...getInfisicalRuntimeSecretStatus(YOUTUBE_API_KEY), present: !!value } });
  } catch (err) {
    return c.json({ ok: false, error: (err as Error).message }, 500);
  }
});

tuningRoutes.put("/admin/tuning/secrets/youtube-api-key", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>;
  const apiKey = typeof body.apiKey === "string" ? body.apiKey.trim() : "";
  if (!apiKey) return c.json({ ok: false, error: "apiKey required" }, 400);
  try {
    await setInfisicalRuntimeSecret(YOUTUBE_API_KEY, apiKey);
    return c.json({ ok: true, youtubeApiKey: getInfisicalRuntimeSecretStatus(YOUTUBE_API_KEY) });
  } catch (err) {
    return c.json({ ok: false, error: (err as Error).message }, 500);
  }
});

tuningRoutes.post("/admin/tuning/secrets/youtube-api-key/gcloud", async (c) => {
  const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>;
  const secret =
    typeof body.secret === "string" && body.secret.trim()
      ? body.secret.trim()
      : "discutere-youtube-api-key";
  const version =
    typeof body.version === "string" && body.version.trim() ? body.version.trim() : "latest";
  const project = typeof body.project === "string" && body.project.trim() ? body.project.trim() : undefined;
  try {
    const value = await readGcloudSecret({ secret, version, project });
    if (!value) return c.json({ ok: false, error: "gcloud returned an empty secret value" }, 502);
    await setInfisicalRuntimeSecret(YOUTUBE_API_KEY, value);
    return c.json({ ok: true, secret, version, youtubeApiKey: getInfisicalRuntimeSecretStatus(YOUTUBE_API_KEY) });
  } catch (err) {
    return c.json({ ok: false, error: (err as Error).message }, 500);
  }
});

tuningRoutes.put("/admin/tuning/cost-relay", async (c) => {
  if (!store) return c.json({ error: "settings store not ready" }, 503);
  const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>;
  const patch: Record<string, unknown> = {};
  if (body.enabled !== undefined) patch.enabled = Boolean(body.enabled);
  for (const k of ["anatomiaUrl", "concordiaUrl", "service"] as const) {
    if (body[k] !== undefined) patch[k] = String(body[k]);
  }
  const effective = store.setCostRelay(patch);
  return c.json({ ok: true, costRelay: effective });
});

tuningRoutes.put("/admin/tuning/facilitator", async (c) => {
  if (!store) return c.json({ error: "settings store not ready" }, 503);
  const body = (await c.req.json().catch(() => ({}))) as Record<string, unknown>;
  const patch: Record<string, unknown> = {};
  for (const k of ["tickMs", "idleGapMs", "maxPersonas", "aufhebungTarget"] as const) {
    if (body[k] !== undefined) patch[k] = Number(body[k]);
  }
  if (body.convergePolicy !== undefined) patch.convergePolicy = body.convergePolicy;
  const effective = store.setFacilitatorTuning(patch);
  return c.json({ ok: true, facilitator: effective });
});

tuningRoutes.put("/admin/tuning/role", async (c) => {
  if (!store) return c.json({ error: "settings store not ready" }, 503);
  const body = (await c.req.json().catch(() => ({}))) as { role?: string; text?: string | null };
  if (!body.role) return c.json({ error: "role required" }, 400);
  store.setRolePrompt(body.role, body.text ?? null);
  return c.json({ ok: true, role: body.role, effective: store.getRolePrompt(body.role) });
});

tuningRoutes.put("/admin/tuning/rule", async (c) => {
  if (!store) return c.json({ error: "settings store not ready" }, 503);
  const body = (await c.req.json().catch(() => ({}))) as { ruleId?: string; text?: string | null };
  if (!body.ruleId) return c.json({ error: "ruleId required" }, 400);
  store.setRuleInstruction(body.ruleId, body.text ?? null);
  reseedRules?.(); // 稼働中の rule engine に instructions を反映する。
  return c.json({ ok: true, ruleId: body.ruleId, effective: store.getRuleInstruction(body.ruleId) });
});

tuningRoutes.get("/admin/tuning", (c) => c.html(TUNING_HTML));

const TUNING_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>Discutere 議論設定</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--muted:#64748b;--muted-2:#94a3b8;--surface:#fff;--field:#fff;--surface-soft:#f1f5f9;--border:#e2e8f0;--border-strong:#cbd5e1;--primary:#2563eb;--primary-fg:#fff;--secondary:#e2e8f0;--secondary-fg:#334155;--ok:#16a34a;--link:#2563eb}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--muted:#8b94a3;--muted-2:#6b7280;--surface:#161922;--field:#0c0e13;--surface-soft:#1a1f2b;--border:#232733;--border-strong:#2a2f3a;--primary:#2563eb;--primary-fg:#fff;--secondary:#2a2f3a;--secondary-fg:#cbd3e1;--ok:#22c55e;--link:#7aa2ff}}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:var(--fg)}
 .wrap{max-width:880px;margin:0 auto;padding:24px}
 a{color:var(--link)} h1{font-size:18px;margin:0 0 4px} h2{font-size:15px;margin:22px 0 8px;color:var(--fg)}
 .sub{color:var(--muted);font-size:13px;margin:0 0 14px}
 .card{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:14px 16px;margin-bottom:14px}
 label{display:block;font-size:12px;color:var(--muted);margin:8px 0 3px}
 input,select,textarea{width:100%;box-sizing:border-box;background:var(--field);color:var(--fg);border:1px solid var(--border-strong);border-radius:7px;padding:8px 10px;font-size:13px;font-family:inherit}
 textarea{min-height:74px;resize:vertical;line-height:1.5}
 .row{display:flex;gap:12px;flex-wrap:wrap} .row>div{flex:1;min-width:120px}
 button{border:0;border-radius:8px;padding:8px 15px;font-size:13px;cursor:pointer;background:var(--primary);color:var(--primary-fg);margin-top:10px}
 button.ghost{background:var(--secondary);color:var(--secondary-fg)} button:hover{filter:brightness(1.1)}
 .muted{color:var(--muted-2);font-size:12px} .ok{color:var(--ok)} .name{color:var(--fg);font-weight:600}
 .def{color:var(--muted-2);font-size:11px;white-space:pre-wrap;margin-top:4px}
 .bar{margin-bottom:10px}
</style></head><body><div class="wrap">
 <div class="bar"><a href="/">← トップ</a></div>
 <h1>議論設定</h1>
 <p class="sub">facilitator の収束トリガー・役割プロンプト・debate ルールを再起動なしで調整します。保存すると即時 (facilitator は次 tick、役割プロンプトはワーカー再起動時) に反映されます。</p>

 <h2>収束トリガー (facilitator)</h2>
 <div class="card">
   <div class="row">
     <div><label>収束する参加人数 (maxPersonas)</label><input id="maxPersonas" type="number" min="1"></div>
     <div><label>収束する止揚数 (aufhebungTarget)</label><input id="aufhebungTarget" type="number" min="1"></div>
   </div>
   <div class="row">
     <div><label>idle 介入 (idleGapMs)</label><input id="idleGapMs" type="number" min="1000"></div>
     <div><label>tick 周期 (tickMs ※次回起動で反映)</label><input id="tickMs" type="number" min="1000"></div>
   </div>
   <label>収束ポリシー</label>
   <select id="convergePolicy">
     <option value="default">default — 止揚 N 件 または 参加人数超過 で収束</option>
     <option value="aufhebung-only">aufhebung-only — 人数では収束せず、止揚が出るまで続ける</option>
   </select>
   <button onclick="saveFacil()">収束トリガーを保存</button>
   <span id="facMsg" class="ok"></span>
 </div>

 <h2>コスト relay (Anatomia / Concordia へ Push)</h2>
 <div class="card">
   <p class="muted" style="margin:0 0 8px">LLM コストの累積を各サービスの cost-feed パネルへ送る。有効化・送信先・service ラベルは即時反映 (次 tick)。送信周期 (interval) は config 由来で変更は再起動。空 URL のサービスへは送りません。</p>
   <label><input id="cr_enabled" type="checkbox" style="width:auto;margin-right:6px;vertical-align:middle">定期 relay を有効化</label>
   <div class="row">
     <div><label>Anatomia URL (→ /api/cost-feed)</label><input id="cr_anatomiaUrl" type="text" placeholder="http://localhost:4200"></div>
     <div><label>Concordia URL (→ /v1/cost-feed)</label><input id="cr_concordiaUrl" type="text" placeholder="http://localhost:17330"></div>
   </div>
   <label>service ラベル</label><input id="cr_service" type="text" placeholder="discutere">
   <button onclick="saveCostRelay()">コスト relay を保存</button>
   <span id="crMsg" class="ok"></span>
 </div>

 <h2>YouTube API key</h2>
 <div class="card">
   <p class="muted" style="margin:0 0 8px">Infisical に保存します。保存・再読込後は再起動なしで、次の YouTube crawl から使われます。key 本文は表示しません。</p>
   <div class="muted" id="yt_status">loading...</div>
   <label>API key を直接保存</label>
   <input id="yt_apiKey" type="password" autocomplete="off" placeholder="AIza...">
   <button onclick="saveYoutubeKey()">保存して即時反映</button>
   <button class="ghost" onclick="refreshYoutubeKey()">Infisical から再読込</button>
   <span id="ytMsg" class="ok"></span>
   <div class="row">
     <div><label>gcloud secret 名</label><input id="yt_gcloud_secret" type="text" value="discutere-youtube-api-key"></div>
     <div><label>GCP project (任意)</label><input id="yt_gcloud_project" type="text" placeholder="project-id"></div>
     <div><label>version</label><input id="yt_gcloud_version" type="text" value="latest"></div>
   </div>
   <button onclick="importYoutubeKeyFromGcloud()">gcloud から取得して保存</button>
   <span id="ytGcloudMsg" class="ok"></span>
 </div>

 <h2>役割プロンプト</h2>
 <div id="roles"></div>

 <h2>debate ルール (instructions)</h2>
 <div id="rules"></div>
</div>
<script>
let DATA=null;
function esc(s){return (s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
async function load(){
  const r=await fetch('/api/admin/tuning/data'); DATA=await r.json();
  if(!DATA.enabled){ document.querySelector('.wrap').innerHTML='<p class="muted">settings store 未準備 (persona-engine 起動前)。</p>'; return; }
  const f=DATA.facilitator;
  for(const k of ['maxPersonas','aufhebungTarget','idleGapMs','tickMs']) document.getElementById(k).value=f[k];
  document.getElementById('convergePolicy').value=f.convergePolicy;
  const cr=DATA.costRelay||{};
  document.getElementById('cr_enabled').checked=!!cr.enabled;
  document.getElementById('cr_anatomiaUrl').value=cr.anatomiaUrl||'';
  document.getElementById('cr_concordiaUrl').value=cr.concordiaUrl||'';
  document.getElementById('cr_service').value=cr.service||'';
  renderYoutubeStatus(DATA.secrets&&DATA.secrets.youtubeApiKey);
  document.getElementById('roles').innerHTML=DATA.rolePrompts.map((p,i)=>roleCard(p,i)).join('');
  document.getElementById('rules').innerHTML=DATA.ruleInstructions.map((p,i)=>ruleCard(p,i)).join('');
}
function renderYoutubeStatus(s){
  const e=document.getElementById('yt_status'); if(!e) return;
  if(!s){ e.textContent='status unavailable'; return; }
  e.innerHTML='Infisical bootstrap: '+(s.bootstrapConfigured?'<span class="ok">ready</span>':'missing')+
    ' / runtime cache: '+(s.cached?'loaded':'not loaded')+
    ' / key: '+(s.present?'<span class="ok">configured</span>':'not configured');
}
function roleCard(p,i){
  const role=p.key.replace(/^rolePrompt:/,'');
  return '<div class="card"><div class="name">'+esc(role)+(p.override?' <span class="ok">(override)</span>':'')+'</div>'+
    '<textarea id="role_'+i+'">'+esc(p.effective)+'</textarea>'+
    '<div class="def">既定: '+esc(p.default)+'</div>'+
    '<button onclick="saveRole(\\''+esc(role)+'\\','+i+')">保存</button> '+
    '<button class="ghost" onclick="resetRole(\\''+esc(role)+'\\')">既定に戻す</button>'+
    ' <span id="roleMsg_'+i+'" class="ok"></span></div>';
}
function ruleCard(p,i){
  const id=p.key.replace(/^ruleInstruction:/,'');
  return '<div class="card"><div class="name">'+esc(id)+(p.override?' <span class="ok">(override)</span>':'')+'</div>'+
    '<textarea id="rule_'+i+'">'+esc(p.effective)+'</textarea>'+
    '<div class="def">既定: '+esc(p.default)+'</div>'+
    '<button onclick="saveRule(\\''+esc(id)+'\\','+i+')">保存</button> '+
    '<button class="ghost" onclick="resetRule(\\''+esc(id)+'\\')">既定に戻す</button>'+
    ' <span id="ruleMsg_'+i+'" class="ok"></span></div>';
}
async function put(url,body){ const r=await fetch(url,{method:'PUT',headers:{'content-type':'application/json'},body:JSON.stringify(body)}); return r.json(); }
async function saveFacil(){
  const body={};
  for(const k of ['maxPersonas','aufhebungTarget','idleGapMs','tickMs']) body[k]=Number(document.getElementById(k).value);
  body.convergePolicy=document.getElementById('convergePolicy').value;
  await put('/api/admin/tuning/facilitator',body);
  flash('facMsg','保存しました'); load();
}
async function saveCostRelay(){
  const body={
    enabled:document.getElementById('cr_enabled').checked,
    anatomiaUrl:document.getElementById('cr_anatomiaUrl').value,
    concordiaUrl:document.getElementById('cr_concordiaUrl').value,
    service:document.getElementById('cr_service').value,
  };
  await put('/api/admin/tuning/cost-relay',body);
  flash('crMsg','保存しました (次 tick で反映)'); load();
}
async function saveYoutubeKey(){
  const apiKey=document.getElementById('yt_apiKey').value.trim();
  if(!apiKey){ flash('ytMsg','apiKey required'); return; }
  const r=await put('/api/admin/tuning/secrets/youtube-api-key',{apiKey});
  if(!r.ok){ flash('ytMsg',r.error||'failed'); return; }
  document.getElementById('yt_apiKey').value='';
  flash('ytMsg','保存しました。次の crawl から反映されます'); load();
}
async function refreshYoutubeKey(){
  const r=await fetch('/api/admin/tuning/secrets/youtube-api-key/refresh',{method:'POST'});
  const j=await r.json();
  if(!j.ok){ flash('ytMsg',j.error||'failed'); return; }
  flash('ytMsg','再読込しました'); load();
}
async function importYoutubeKeyFromGcloud(){
  const body={
    secret:document.getElementById('yt_gcloud_secret').value,
    project:document.getElementById('yt_gcloud_project').value,
    version:document.getElementById('yt_gcloud_version').value,
  };
  const r=await fetch('/api/admin/tuning/secrets/youtube-api-key/gcloud',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});
  const j=await r.json();
  if(!j.ok){ flash('ytGcloudMsg',j.error||'failed'); return; }
  flash('ytGcloudMsg','gcloud から保存しました。次の crawl から反映されます'); load();
}
async function saveRole(role,i){ await put('/api/admin/tuning/role',{role,text:document.getElementById('role_'+i).value}); flash('roleMsg_'+i,'保存'); load(); }
async function resetRole(role){ await put('/api/admin/tuning/role',{role,text:null}); load(); }
async function saveRule(id,i){ await put('/api/admin/tuning/rule',{ruleId:id,text:document.getElementById('rule_'+i).value}); flash('ruleMsg_'+i,'保存 (再シード)'); load(); }
async function resetRule(id){ await put('/api/admin/tuning/rule',{ruleId:id,text:null}); load(); }
function flash(id,msg){ const e=document.getElementById(id); if(e){ e.textContent=msg; setTimeout(()=>{e.textContent='';},1800); } }
load();
</script></body></html>`;

export { tuningRoutes };
