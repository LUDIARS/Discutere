/**
 * トップページ (GET /) — Discutere の各 Web UI への入口。
 *
 * Discutere は server-rendered な埋め込み HTML の UI が複数あるが、 これまで
 * それらを束ねる玄関が無かった。 ここで全 UI へのリンクと簡易ステータスを出す。
 *
 *   /                       このトップページ
 *   /flow                   議論 (ペーパー付き議論フロー)
 *   /chat                   チャット議論 (軽量チャット)
 *   /learning               学習ビュー (基礎知識 / ゲーム学習 / 話題と意見)
 *   /api/admin/dashboard    persona-engine 管制 (kill switch / rule log)
 *   /api/worker-pool        常駐ワーカー制御 (起動/停止)
 *   /api/admin/tuning       議論設定 (収束トリガー / プロンプト / ルール)
 *
 * loopback 運用前提で guard 無し (他の :3100 UI と同じ)。
 */

import { Hono } from "hono";

const topPageRoutes = new Hono();

topPageRoutes.get("/", (c) => c.html(TOP_HTML));

const TOP_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>Discutere</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--muted:#64748b;--muted-2:#94a3b8;--surface:#fff;--surface-soft:#f1f5f9;--border:#e2e8f0;--primary:#2563eb}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--muted:#8b94a3;--muted-2:#6b7280;--surface:#161922;--surface-soft:#1a1f2b;--border:#232733;--primary:#2563eb}}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:var(--fg)}
 .wrap{max-width:780px;margin:0 auto;padding:40px 24px}
 h1{font-size:22px;margin:0 0 2px} .sub{color:var(--muted);font-size:13px;margin:0 0 26px}
 h2{font-size:13px;margin:26px 0 10px;color:var(--muted);font-weight:700}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:14px}
 a.card{display:block;text-decoration:none;background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:16px 18px;color:var(--fg);transition:.15s}
 a.card:hover{border-color:var(--primary);background:var(--surface-soft)}
 .t{font-size:15px;font-weight:600;margin-bottom:4px} .d{font-size:12px;color:var(--muted);line-height:1.5}
 .status{margin-top:24px;font-size:12px;color:var(--muted-2)}
 .pill{display:inline-block;background:var(--surface-soft);border-radius:6px;padding:2px 8px;margin-right:6px;color:var(--muted)}
 .foot{margin-top:30px;color:var(--muted-2);font-size:12px}
</style></head><body><div class="wrap">
 <h1>Discutere</h1>
 <p class="sub">遊びの議論プラットフォーム — 各 UI の入口</p>
 <h2>議論</h2>
 <div class="grid">
   <a class="card" href="/flow?scope=ai"><div class="t">AI議論</div><div class="d">AIだけで議論をする</div></a>
   <a class="card" href="/chat"><div class="t">チャット議論</div><div class="d">人間とAIで議論をする(整備中)</div></a>
   <a class="card" href="/guide"><div class="t">使い方ガイド</div><div class="d">Discord フォーラム / チャンネルの使い方とルール</div></a>
 </div>
 <h2>設定</h2>
 <div class="grid">
   <a class="card" href="/api/admin/tuning"><div class="t">議論設定</div><div class="d">収束トリガー (20 等) / 役割プロンプト / debate ルール</div></a>
   <a class="card" href="/api/worker-pool"><div class="t">常駐ワーカー制御</div><div class="d">議論ペルソナの起動 / 停止 (サブスク枠管理)</div></a>
   <a class="card" href="/api/admin/noise"><div class="t">ノイズ管理</div><div class="d">議論データから別ゲーム/煽り等のノイズ発話を除外</div></a>
   <a class="card" href="/api/admin/consensus"><div class="t">合意スコア</div><div class="d">各意見 (人間/AI 同一) の合意度を AI が採点・👍 可視化</div></a>
   <a class="card" href="/api/admin/dashboard"><div class="t">管制ダッシュボード</div><div class="d">persona-engine 状態・kill switch・rule log</div></a>
   <a class="card" href="/api/admin/personas"><div class="t">匿名ペルソナ管理</div><div class="d">Voluptas からの匿名取込 / 公開レビュー話者の採用</div></a>
 </div>
 <h2>学習データ</h2>
 <div class="grid">
   <a class="card" href="/learning"><div class="t">学習</div><div class="d">ゲーム内容とユーザの反応を学習し、蓄積データを閲覧</div></a>
   <a class="card" href="/api/admin/consensus"><div class="t">合意スコア</div><div class="d">各意見 (人間/AI 同一) の合意度を AI が採点・👍 可視化</div></a>
 </div>
 <div class="status" id="status"><span class="pill">読み込み中…</span></div>
 <div class="foot">LUDIARS / Discutere — loopback :3100</div>
</div>
<script>
async function status(){
  const el=document.getElementById('status'); const parts=[];
  try{ const w=await (await fetch('/api/worker-pool/status')).json();
    if(w.enabled){ const run=w.workers.filter(x=>x.running||x.registered).length; parts.push('<span class="pill">worker-pool: '+run+'/'+w.workers.length+' 稼働</span>'); }
    else parts.push('<span class="pill">backend: facilitator</span>');
  }catch(e){}
  try{ const t=await (await fetch('/api/admin/tuning/data')).json();
    if(t.enabled){ const f=t.facilitator; parts.push('<span class="pill">収束: '+f.maxPersonas+'人 / 止揚'+f.aufhebungTarget+'件</span>'); parts.push('<span class="pill">policy: '+f.convergePolicy+'</span>'); }
  }catch(e){}
  el.innerHTML=parts.join('')||'<span class="pill">status 取得不可</span>';
}
status();
</script></body></html>`;

export { topPageRoutes };
