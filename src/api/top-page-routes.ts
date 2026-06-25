/**
 * トップページ (GET /) — Discutere の各 Web UI への入口。
 *
 * Discutere は server-rendered な埋め込み HTML の UI が複数あるが、 これまで
 * それらを束ねる玄関が無かった。 ここで全 UI へのリンクと簡易ステータスを出す。
 *
 *   /                       このトップページ
 *   /learning               学習ビュー (基礎知識 / ゲーム学習 / 話題と意見)
 *   /api/admin/dashboard    persona-engine 管制 (kill switch / rule log)
 *   /api/worker-pool        常駐ワーカー制御 (起動/停止)
 *   /api/admin/tuning       議論チューニング (収束トリガー / プロンプト / ルール)
 *
 * loopback 運用前提で guard 無し (他の :3100 UI と同じ)。
 */

import { Hono } from "hono";

const topPageRoutes = new Hono();

topPageRoutes.get("/", (c) => c.html(TOP_HTML));

const TOP_HTML = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Discutere</title>
<style>
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:#0f1115;color:#e6e6e6}
 .wrap{max-width:780px;margin:0 auto;padding:40px 24px}
 h1{font-size:22px;margin:0 0 2px} .sub{color:#8b94a3;font-size:13px;margin:0 0 26px}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:14px}
 a.card{display:block;text-decoration:none;background:#161922;border:1px solid #232733;border-radius:12px;padding:16px 18px;color:#e6e6e6;transition:.15s}
 a.card:hover{border-color:#2563eb;background:#1a1f2b}
 .t{font-size:15px;font-weight:600;margin-bottom:4px} .d{font-size:12px;color:#8b94a3;line-height:1.5}
 .status{margin-top:24px;font-size:12px;color:#6b7280}
 .pill{display:inline-block;background:#222633;border-radius:6px;padding:2px 8px;margin-right:6px;color:#9aa3b2}
 .foot{margin-top:30px;color:#4b5563;font-size:12px}
</style></head><body><div class="wrap">
 <h1>Discutere</h1>
 <p class="sub">遊びの議論プラットフォーム — 各 UI の入口</p>
 <div class="grid">
   <a class="card" href="/flow"><div class="t">チャット議論</div><div class="d">ディスカッションペーパーとターン数を設定 → 自動で議論が進行し、ペーパーが更新されていく</div></a>
   <a class="card" href="/chat"><div class="t">フリーチャット</div><div class="d">Discord 非依存の軽量チャットで AI 論者と自由に議論する</div></a>
   <a class="card" href="/guide"><div class="t">使い方ガイド</div><div class="d">Discord フォーラム / チャンネルの使い方とルール</div></a>
   <a class="card" href="/learning"><div class="t">学習ビュー</div><div class="d">基礎知識 / ゲーム学習 / 話題と意見の蓄積を閲覧</div></a>
   <a class="card" href="/api/admin/dashboard"><div class="t">管制ダッシュボード</div><div class="d">persona-engine 状態・kill switch・rule log</div></a>
   <a class="card" href="/api/worker-pool"><div class="t">常駐ワーカー制御</div><div class="d">議論ペルソナの起動 / 停止 (サブスク枠管理)</div></a>
   <a class="card" href="/api/admin/tuning"><div class="t">議論チューニング</div><div class="d">収束トリガー (20 等) / 役割プロンプト / debate ルール</div></a>
   <a class="card" href="/api/admin/personas"><div class="t">ペルソナ生成</div><div class="d">クロール後の自動採用 / C1 採用 / C2 合成生成 / 母数推定</div></a>
   <a class="card" href="/api/admin/noise"><div class="t">ノイズ管理</div><div class="d">議論データから別ゲーム/煽り等のノイズ発話を除外</div></a>
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
