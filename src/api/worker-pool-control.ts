/**
 * 常駐ワーカープールの制御 UI / API (port 3100)。
 *
 * boot 時自動 spawn ではなく、ここから手動で「何体・いつ起動/停止」を制御する。
 * 8 体一斉起動でサブスク枠を食い尽くす事故 (2026-06-06) の再発防止。
 *
 *   GET  /api/worker-pool            制御 HTML
 *   GET  /api/worker-pool/status     JSON 状態
 *   POST /api/worker-pool/start      全ワーカー起動
 *   POST /api/worker-pool/stop       全ワーカー停止 (kill)
 *   POST /api/worker-pool/worker/:id/start
 *   POST /api/worker-pool/worker/:id/stop
 *
 * WorkerPool は index.ts で setWorkerPoolControl() で注入する (backend=worker-pool 時)。
 */

import { Hono } from "hono";

import type { WorkerPool } from "../persona-engine/worker-pool/pool.js";

let pool: WorkerPool | null = null;

export function setWorkerPoolControl(p: WorkerPool | null): void {
  pool = p;
}

const workerPoolControlRoutes = new Hono();

workerPoolControlRoutes.get("/worker-pool/status", (c) => {
  if (!pool) return c.json({ enabled: false, workers: [] });
  return c.json({ enabled: true, workers: pool.listConfigured() });
});

workerPoolControlRoutes.post("/worker-pool/start", (c) => {
  if (!pool) return c.json({ error: "worker pool not enabled (backend != worker-pool)" }, 503);
  pool.start();
  return c.json({ ok: true, workers: pool.listConfigured() });
});

workerPoolControlRoutes.post("/worker-pool/stop", (c) => {
  if (!pool) return c.json({ error: "worker pool not enabled" }, 503);
  pool.stop();
  return c.json({ ok: true });
});

workerPoolControlRoutes.post("/worker-pool/worker/:id/start", (c) => {
  if (!pool) return c.json({ error: "worker pool not enabled" }, 503);
  const ok = pool.startWorker(c.req.param("id"));
  return c.json({ ok }, ok ? 200 : 404);
});

workerPoolControlRoutes.post("/worker-pool/worker/:id/stop", (c) => {
  if (!pool) return c.json({ error: "worker pool not enabled" }, 503);
  const ok = pool.stopWorker(c.req.param("id"));
  return c.json({ ok }, ok ? 200 : 404);
});

workerPoolControlRoutes.get("/worker-pool", (c) => c.html(CONTROL_HTML));

const CONTROL_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>Discutere 常駐ワーカー制御</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--muted:#64748b;--muted-2:#94a3b8;--surface:#fff;--surface-soft:#f1f5f9;--border:#e2e8f0;--primary:#2563eb;--danger:#b91c1c;--secondary:#e2e8f0;--secondary-hover:#cbd5e1;--secondary-fg:#334155}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--muted:#8b94a3;--muted-2:#6b7280;--surface:#161922;--surface-soft:#222633;--border:#232733;--primary:#2563eb;--danger:#b91c1c;--secondary:#2a2f3a;--secondary-hover:#353b48;--secondary-fg:#e6e6e6}}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:var(--fg)}
 .wrap{max-width:880px;margin:0 auto;padding:24px}
 h1{font-size:18px;margin:0 0 4px} .sub{color:var(--muted);font-size:13px;margin:0 0 18px}
 .bar{display:flex;gap:10px;margin-bottom:16px}
 button{border:0;border-radius:8px;padding:9px 16px;font-size:13px;cursor:pointer;background:var(--secondary);color:var(--secondary-fg)}
 button:hover{background:var(--secondary-hover)}
 button.primary{background:var(--primary);color:#fff} button.danger{background:var(--danger);color:#fff}
 button.sm{padding:5px 11px;font-size:12px}
 table{width:100%;border-collapse:collapse;background:var(--surface);border-radius:10px;overflow:hidden}
 th,td{text-align:left;padding:10px 12px;border-bottom:1px solid var(--border);font-size:13px}
 th{color:var(--muted);font-weight:600;font-size:12px}
 .dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:7px;vertical-align:middle}
 .on{background:#22c55e}.reg{background:#3b82f6}.off{background:#4b5563}
 .muted{color:var(--muted-2)}.pill{font-size:11px;color:var(--muted);background:var(--surface-soft);border-radius:6px;padding:2px 7px}
</style></head><body><div class="wrap">
 <h1>常駐ワーカー制御</h1>
 <p class="sub">議論ペルソナをサブスク Lictor セッションとして手動起動/停止します。サブスク枠 (目安 8 セッション) を意識して必要数だけ起動してください。</p>
 <div class="bar">
   <button class="primary" onclick="act('/api/worker-pool/start')">全部 起動</button>
   <button class="danger" onclick="act('/api/worker-pool/stop')">全部 停止</button>
   <button onclick="refresh()">更新</button>
   <span id="count" class="pill" style="align-self:center"></span>
 </div>
 <table><thead><tr><th>状態</th><th>ペルソナ</th><th>provider / model</th><th>port</th><th></th></tr></thead>
 <tbody id="rows"><tr><td colspan="5" class="muted">読み込み中…</td></tr></tbody></table>
 <p class="sub" style="margin-top:14px">凡例: <span class="dot on"></span>起動中 <span class="dot reg"></span>登録済(発話可) <span class="dot off"></span>停止</p>
</div>
<script>
async function refresh(){
  const r=await fetch('/api/worker-pool/status'); const j=await r.json();
  const tb=document.getElementById('rows'); tb.innerHTML='';
  if(!j.enabled){ tb.innerHTML='<tr><td colspan="5" class="muted">worker-pool backend が無効です (config.llm.backend=worker-pool で起動してください)</td></tr>'; document.getElementById('count').textContent=''; return; }
  let run=0,reg=0;
  for(const w of j.workers){
    if(w.running)run++; if(w.registered)reg++;
    const cls=w.registered?'reg':(w.settling?'on':(w.running?'on':'off'));
    const stat=w.registered?'登録済':(w.settling?'登録待機中':(w.running?'起動中':'停止'));
    const btn=w.running?'<button class="sm danger" onclick="act(\\'/api/worker-pool/worker/'+w.id+'/stop\\')">停止</button>'
                        :'<button class="sm primary" onclick="act(\\'/api/worker-pool/worker/'+w.id+'/start\\')">起動</button>';
    tb.insertAdjacentHTML('beforeend','<tr><td><span class="dot '+cls+'"></span>'+stat+(w.busy?' <span class="pill">busy</span>':'')+'</td><td>'+w.role+' <span class="muted">'+w.id+'</span></td><td class="muted">'+w.provider+' / '+w.model+'</td><td class="muted">'+(w.port??'-')+'</td><td>'+btn+'</td></tr>');
  }
  document.getElementById('count').textContent=run+' 起動 / '+reg+' 登録 / '+j.workers.length+' 定義';
}
async function act(url){ await fetch(url,{method:'POST'}); setTimeout(refresh,400); }
refresh(); setInterval(refresh,3000);
</script></body></html>`;

export { workerPoolControlRoutes };
