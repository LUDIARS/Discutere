/** Loopback-only administration for externally sourced anonymous personas. */

import { existsSync, readFileSync, writeFileSync } from "node:fs";
import path from "node:path";
import { Hono } from "hono";

import { getConfig, _resetConfig } from "../config.js";
import { runAdoptFromKg } from "../flow/persona-adopt-runner.js";
import { importVoluptasPersonas } from "../flow/persona-import.js";
import { parsePersonaDocument } from "../flow/persona-import-document.js";
import { listPoolPersonas } from "../flow/persona-pool.js";

// @spec インポート (Vo → Di)
const personaRoutes = new Hono();

function configPath(): string {
  return path.resolve(process.env.DISCUTERE_CONFIG ?? "./discutere.config.json");
}

function readConfigFile(): Record<string, unknown> {
  const file = configPath();
  if (!existsSync(file)) return {};
  return JSON.parse(readFileSync(file, "utf8")) as Record<string, unknown>;
}

function writeFlowConfig(patch: Record<string, unknown>): void {
  const current = readConfigFile();
  const flow = (current.flow && typeof current.flow === "object" ? current.flow : {}) as Record<string, unknown>;
  current.flow = { ...flow, ...patch };
  writeFileSync(configPath(), `${JSON.stringify(current, null, 2)}\n`, "utf8");
  _resetConfig();
}

function boolValue(value: unknown, fallback: boolean): boolean {
  if (typeof value === "boolean") return value;
  if (typeof value === "string") return /^(1|true|yes|on)$/i.test(value.trim());
  return fallback;
}

function boundedInteger(value: unknown, fallback: number, min: number, max: number): number {
  const number = typeof value === "number" ? value : Number(value);
  if (!Number.isFinite(number)) return fallback;
  return Math.max(min, Math.min(max, Math.floor(number)));
}

personaRoutes.get("/admin/personas", (context) => context.html(PERSONA_HTML));

personaRoutes.get("/admin/personas/data", (context) => {
  const config = getConfig();
  return context.json({
    autoAdoptOnCrawl: config.flow.autoAdoptOnCrawl,
    envOverride: process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL ?? null,
    configPath: configPath(),
    imported: listPoolPersonas({ origin: "imported" }).length,
    adopted: listPoolPersonas({ origin: "adopted" }).length,
  });
});

personaRoutes.put("/admin/personas/auto-adopt", async (context) => {
  const body = (await context.req.json().catch(() => ({}))) as { enabled?: unknown };
  if (body.enabled === undefined) return context.json({ error: "enabled required" }, 400);
  try {
    writeFlowConfig({ autoAdoptOnCrawl: boolValue(body.enabled, false) });
  } catch (error) {
    return context.json({ error: `config write failed: ${(error as Error).message}` }, 500);
  }
  return context.json({
    ok: true,
    autoAdoptOnCrawl: getConfig().flow.autoAdoptOnCrawl,
    envOverride: process.env.DISCUTERE_FLOW_AUTO_ADOPT_ON_CRAWL ?? null,
    configPath: configPath(),
  });
});

// 本文は JSON 配列 / { personas: [] } / NDJSON のいずれでもよい。CLI (`npm run persona:import`) と
// 同じ parsePersonaDocument で解釈し、壊れた行は落とさず skip 件数として返す。
personaRoutes.post("/admin/personas/import", async (context) => {
  const body = await context.req.text().catch(() => "");
  try {
    const document = parsePersonaDocument(body);
    return context.json({
      ok: true,
      ...importVoluptasPersonas(document.personas, { invalidJsonLines: document.invalidJsonLines }),
    });
  } catch (error) {
    return context.json({ error: (error as Error).message }, 400);
  }
});

personaRoutes.post("/admin/personas/adopt", async (context) => {
  const body = (await context.req.json().catch(() => ({}))) as {
    source?: unknown;
    minOpinions?: unknown;
    dry?: unknown;
  };
  const sourceFilter = typeof body.source === "string" && body.source.trim() ? body.source.trim() : undefined;
  const minOpinions = boundedInteger(body.minOpinions, 10, 1, 10_000);
  const dry = boolValue(body.dry, true);
  try {
    const summary = runAdoptFromKg({ sourceFilter, minOpinions, dry });
    return context.json({ ok: true, dry, sourceFilter: sourceFilter ?? null, minOpinions, ...summary });
  } catch (error) {
    return context.json({ error: (error as Error).message }, 500);
  }
});

const PERSONA_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>Discutere 匿名ペルソナ管理</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--muted:#64748b;--surface:#fff;--field:#fff;--border:#e2e8f0;--border-strong:#cbd5e1;--primary:#2563eb;--secondary:#e2e8f0;--secondary-fg:#334155;--ok:#16a34a;--bad:#dc2626;--link:#2563eb}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--muted:#8b94a3;--surface:#161922;--field:#0c0e13;--border:#232733;--border-strong:#2a2f3a;--primary:#2563eb;--secondary:#2a2f3a;--secondary-fg:#cbd3e1;--ok:#22c55e;--bad:#f87171;--link:#7aa2ff}}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:var(--fg)}
 .wrap{max-width:820px;margin:0 auto;padding:24px}a{color:var(--link)}h1{font-size:20px;margin:0 0 6px}h2{font-size:15px;margin:0 0 8px}
 .sub,.muted{color:var(--muted);font-size:13px}.card{background:var(--surface);border:1px solid var(--border);border-radius:8px;padding:14px 16px;margin:14px 0}
 label{display:block;font-size:12px;color:var(--muted);margin:8px 0 3px}input{width:100%;box-sizing:border-box;background:var(--field);color:var(--fg);border:1px solid var(--border-strong);border-radius:7px;padding:8px 10px;font-size:13px}
 .row{display:flex;gap:12px;flex-wrap:wrap}.row>div{flex:1;min-width:150px}.switch{display:flex;align-items:center;gap:10px}.switch input{width:auto}
 button{border:0;border-radius:8px;padding:8px 15px;font-size:13px;cursor:pointer;background:var(--primary);color:#fff;margin-top:10px}.ok{color:var(--ok)}.bad{color:var(--bad)}
 pre{white-space:pre-wrap;background:var(--field);border:1px solid var(--border);border-radius:8px;padding:10px;max-height:300px;overflow:auto}
</style></head><body><div class="wrap">
 <p><a href="/">トップ</a></p>
 <h1>匿名ペルソナ管理</h1>
 <p class="sub">Di はペルソナやアンケートを生成しません。Voluptas の仮名 user_id 付き export、または公開レビュー話者だけを取り込みます。</p>
 <div class="card">
   <h2>Voluptas から取り込む</h2>
   <p class="muted">Voluptas の <code>export:personas</code> で生成した JSON のみ受理します。生 SID・表示名・メールは保存しません。</p>
   <input id="importFile" type="file" accept="application/json,application/x-ndjson,.json,.jsonl,.ndjson">
   <button onclick="runImport()">匿名ペルソナを取り込む</button>
 </div>
 <div class="card">
   <h2>公開レビュー話者の採用</h2>
   <label class="switch"><input id="auto" type="checkbox"> クロール後に自動採用する</label>
   <p class="muted" id="autoMeta"></p>
   <button onclick="saveAuto()">設定を保存</button> <span id="autoMsg"></span>
   <div class="row"><div><label>source</label><input id="source" placeholder="youtube / steam / 空なら全体"></div><div><label>min opinions</label><input id="minOpinions" type="number" value="10" min="1"></div></div>
   <label class="switch"><input id="dry" type="checkbox" checked> dry-run</label>
   <button onclick="runAdopt()">採用を試す</button>
 </div>
 <pre id="out"></pre>
</div><script>
const byId=id=>document.getElementById(id);
function show(value){byId('out').textContent=typeof value==='string'?value:JSON.stringify(value,null,2)}
async function load(){const data=await fetch('/api/admin/personas/data').then(r=>r.json());byId('auto').checked=!!data.autoAdoptOnCrawl;byId('autoMeta').textContent='imported: '+data.imported+' / adopted: '+data.adopted+' / config: '+data.configPath+(data.envOverride!==null?' / env override: '+data.envOverride:'')}
async function post(url,body){const response=await fetch(url,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});const data=await response.json();show(data);return data}
async function runImport(){
  const file=byId('importFile').files[0];
  if(!file){
    alert('JSON/JSONL file required');
    return;
  }
  const response=await fetch('/api/admin/personas/import',{
    method:'POST',
    headers:{'content-type':'application/x-ndjson'},
    body:await file.text()
  });
  show(await response.json());
  await load();
}
async function saveAuto(){const response=await fetch('/api/admin/personas/auto-adopt',{method:'PUT',headers:{'content-type':'application/json'},body:JSON.stringify({enabled:byId('auto').checked})});const data=await response.json();byId('autoMsg').className=data.ok?'ok':'bad';byId('autoMsg').textContent=data.ok?'保存しました':'失敗';show(data);await load()}
async function runAdopt(){await post('/api/admin/personas/adopt',{source:byId('source').value,minOpinions:Number(byId('minOpinions').value),dry:byId('dry').checked});await load()}
load();
</script></body></html>`;

export { personaRoutes };
