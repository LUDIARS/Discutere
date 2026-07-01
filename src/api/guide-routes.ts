/**
 * 使い方ガイド (GET /guide) — Discord フォーラム / チャンネルの使い方とルール。
 *
 * Bot に初めて触る人が「どこに何を投稿すれば議論が回るか」を 1 ページで掴むための
 * server-rendered な静的ページ。チャンネル名 / 方向タグ等は config 既定で変わるので、
 * 実 config (config.discord) を注入して この稼働インスタンスの実値を表示する。
 *
 * 他の :3100 UI と同じく loopback 前提で guard 無し。
 */

import { Hono } from "hono";
import type { DiscutereConfig } from "../config.js";

function esc(s: string): string {
  return s
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

/** タグ名配列を `「A」「B」` 形式の HTML に整形する。 */
function tagPills(names: string[]): string {
  return names.map((n) => `<code>${esc(n)}</code>`).join(" ");
}

/**
 * 使い方ガイドのルートを作る。 discord = config.discord（チャンネル名 / タグの実値）。
 */
export function createGuideRoutes(discord: DiscutereConfig["discord"]): Hono {
  const guideRoutes = new Hono();

  const f = discord.forum;
  const fb = discord.gameFeedback;
  const plainChannelCount = (discord.discussionChannelIds ?? []).filter(Boolean).length;

  const html = `<!doctype html>
<html lang="ja"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="color-scheme" content="light dark">
<title>使い方ガイド — Discutere</title>
<style>
 :root{color-scheme:light;--bg:#f8fafc;--fg:#111827;--text:#334155;--muted:#64748b;--muted-2:#94a3b8;--surface:#fff;--surface-soft:#f1f5f9;--border:#e2e8f0;--link:#2563eb;--code-bg:#e2e8f0;--code-fg:#1d4ed8;--tag-bg:#dcfce7;--tag-fg:#166534;--tag-border:#bbf7d0;--warn-bg:#fef2f2;--warn-border:#fecaca;--warn-fg:#b91c1c}
 @media (prefers-color-scheme:dark){:root{color-scheme:dark;--bg:#0f1115;--fg:#e6e6e6;--text:#d2d7e0;--muted:#8b94a3;--muted-2:#4b5563;--surface:#161922;--surface-soft:#222633;--border:#232733;--link:#60a5fa;--code-bg:#222633;--code-fg:#9fd2ff;--tag-bg:#1d2a1f;--tag-fg:#86d99a;--tag-border:#2c4233;--warn-bg:#2a1f1f;--warn-border:#4a2f2f;--warn-fg:#f0a3a3}}
 body{font-family:system-ui,'Segoe UI',sans-serif;margin:0;background:var(--bg);color:var(--fg);line-height:1.65}
 .wrap{max-width:820px;margin:0 auto;padding:40px 24px 60px}
 a{color:var(--link)}
 h1{font-size:22px;margin:0 0 2px}
 .sub{color:var(--muted);font-size:13px;margin:0 0 8px}
 .back{font-size:12px;color:var(--muted);text-decoration:none;display:inline-block;margin-bottom:24px}
 h2{font-size:16px;margin:30px 0 10px;padding-bottom:6px;border-bottom:1px solid var(--border)}
 .lead{color:var(--text);font-size:13px;margin:0 0 18px}
 .card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:16px 18px;margin:12px 0}
 .card .t{font-size:14px;font-weight:600;margin-bottom:6px}
 ol,ul{margin:8px 0 8px;padding-left:22px;font-size:13.5px}
 li{margin:5px 0}
 p{font-size:13.5px;color:var(--text);margin:8px 0}
 code{background:var(--code-bg);border-radius:5px;padding:1.5px 7px;color:var(--code-fg);font-size:12.5px}
 .tag{background:var(--tag-bg);color:var(--tag-fg);border:1px solid var(--tag-border);border-radius:6px;padding:1.5px 8px;font-size:12px}
 .warn{background:var(--warn-bg);border-color:var(--warn-border)}
 .warn .t{color:var(--warn-fg)}
 .muted{color:var(--muted);font-size:12px}
 .kv{display:grid;grid-template-columns:140px 1fr;gap:4px 14px;font-size:13px;margin:6px 0}
 .kv .k{color:var(--muted)}
 table{border-collapse:collapse;width:100%;font-size:13px;margin:8px 0}
 td,th{border:1px solid var(--border);padding:6px 9px;text-align:left;vertical-align:top}
 th{background:var(--surface);color:var(--text);font-weight:600}
 .foot{margin-top:36px;color:var(--muted-2);font-size:12px}
</style></head><body><div class="wrap">
 <a class="back" href="/">← Discutere トップ</a>
 <h1>使い方ガイド</h1>
 <p class="sub">Discord のフォーラム / チャンネルで議論を回すための手引き</p>

 <p class="lead">Discutere は <b>遊び（ゲーム）について自走で議論する Bot</b> です。あなたが議題を立てると、
 複数の AI ペルソナ（司会・正論派・否定派・意見屋）が賛否を戦わせ、止揚（アウフヘーベン）で論点を
 一つに収束させて結論を残します。投稿の入口は <b>フォーラム</b> が正路です。</p>

 <h2>① フォーラムで議論を始める（正路）</h2>
 <div class="card">
  <div class="t">手順</div>
  <ol>
   <li>議論カテゴリのフォーラムチャンネル（例: <b>ゲーム議論</b>）で <b>新規ポストを作成</b>する。</li>
   <li><b>タイトルに対象ゲーム名を入れる</b>（例:「モンスターストライクの面白いところ」）。タイトル＝議論の主題になる。</li>
   <li>最初の本文（starter）を書いて投稿すると、その瞬間に議論が起動する。AI の返信と最終まとめは
       <b>そのスレッド内</b>に出る。</li>
  </ol>
  <p class="muted">※ ガイド対象は guild 内の <b>すべてのフォーラムチャンネル</b>（forum.enabled = ${f.enabled ? "ON" : "OFF"}）。個別設定は不要。</p>
 </div>
 <div class="card">
  <div class="t">方向タグで議論の向きを変える</div>
  <p>ポストに付けた<b>タグ</b>で議論の方向が決まる（部分一致）。</p>
  <ul>
   <li><span class="tag">改善提案</span> 系タグ → <b>課題を洗い出して具体的な改善案</b>を出し合って収束。<br>
       <span class="muted">該当タグ名: ${tagPills(f.improvementTagNames)}</span></li>
   <li><span class="tag">面白さ</span> 系タグ → <b>何がどう面白いか</b>、体験の魅力を多角的に語り合って収束。<br>
       <span class="muted">該当タグ名: ${tagPills(f.funTagNames)}</span></li>
   <li><b>タグ無し＝「面白さ」方向が既定。</b></li>
  </ul>
 </div>
 <div class="card warn">
  <div class="t">⚠ 対象ゲームが特定できないと議論は始まらない</div>
  <p>分類器が「どのゲーム / ジャンルの話か」を特定できないポストは、噛み合わないので
  <b>議論を開始せず記録のみ</b>に降格する。タイトルか本文に<b>具体的なゲーム名</b>を必ず入れること。</p>
 </div>

 <h2>② 議論の進み方と収束</h2>
 <ul>
  <li><b>後続の投稿</b>は、進行中の議論への<b>参加発言</b>として取り込まれる（新しい議題は立てない）。</li>
  <li><b>人間の発言が最優先。</b>人が口を挟むと議論にミラーされ、AI が最優先で即応する。収束まとめでも人間意見が重視される。</li>
  <li>facilitator が<b>止揚で論点を一つに収束</b>させると、そのスレッドを <b>ロック＋アーカイブ</b>し、
      結論を <code>${esc(f.summaryChannelName)}</code> チャンネルへ転記する。</li>
  <li>結論一覧は Discord の <code>/discutere-conclusions</code>、または
      <a href="/learning">学習ビュー</a> で閲覧できる。</li>
 </ul>

 <h2>③ 自動で作られる管理チャンネル</h2>
 <p class="muted">Bot に「チャンネルの管理」権限があれば、<code>${esc(f.managedCategoryName)}</code> カテゴリ配下に以下を自動作成する。</p>
 <div class="kv">
  <div class="k">${esc(f.dataLearningChannelName)}</div><div>URL を貼ると外部データのクロール取込に回す入口（→ ④）。</div>
  <div class="k">${esc(f.summaryChannelName)}</div><div>収束した議論のまとめが集まる場所。</div>
 </div>

 <h2>④ データ学習依頼（クロール）</h2>
 <div class="card">
  <p><code>${esc(f.dataLearningChannelName)}</code> チャンネルに<b>URL を貼るだけ</b>で、その先の議論データを取り込み、
  学習ベースに加える（議論は起こさない）。ソースは URL から自動判定。</p>
  <p class="muted">対応: Steam レビュー / YouTube コメント / ニコニコ / 一般 Web 記事 / Fandom / Reddit（reddit は API キー設定時）。</p>
 </div>

 <h2>⑤ ゲーム感想チャンネル（匿名収集）${fb.enabled ? "" : " — 無効"}</h2>
 <p><code>${esc(fb.categoryName)}</code> カテゴリ配下のチャンネルへの投稿は、議論にはせず
 <b>匿名でゲーム感想として収集</b>する（📝 が付く）。誰が書いたかは保存しない。</p>

 <h2>⑥ slash コマンド</h2>
 <table>
  <tr><th>コマンド</th><th>内容</th></tr>
  <tr><td><code>/debate topic:…</code></td><td>議題を指定してパーティ議論を開始（司会＋キーマン＋意見、想定発話数で続行/停止を確認）。</td></tr>
  <tr><td><code>/propose statement:…</code></td><td>跳躍的仮説を提案して自走議論を起こす。</td></tr>
  <tr><td><code>/validate mode:…</code></td><td>仮説を検証する（theory=論理 / emotion=感情）。</td></tr>
  <tr><td><code>/integrate</code> / <code>/reject</code></td><td>検証済み仮説を設計知見として統合 / 棄却。</td></tr>
  <tr><td><code>/discutere-conclusions</code></td><td>収束した結論の一覧（gap 指定で裏の論述データ）。</td></tr>
  <tr><td><code>/discutere-queue</code></td><td>進行中 session / 未処理 gap / 仮説の状況。</td></tr>
  <tr><td><code>/discutere-status</code></td><td>persona-engine の稼働状態（ephemeral）。</td></tr>
  <tr><td><code>/discutere-kill enabled:false</code></td><td>自走の全停止 / 再開（<b>admin 限定</b>）。</td></tr>
  <tr><td><code>/discutere-backup</code></td><td>学習データを今すぐ S3 バックアップ（<b>admin 限定</b>）。</td></tr>
 </table>

 <h2>⑦ ルールと注意</h2>
 <ul>
  <li><b>個人データは保存しない。</b>公開された Discord の識別子と表示名以外は持たない。</li>
  <li>admin コマンドは管理者 allowlist に登録された ID のみ実行できる。</li>
  <li>議題は<b>ゲーム / 遊び</b>に関するものに限る（対象が定まらないと議論は始まらない → ①の⚠）。</li>
  <li>平文チャンネルでの議論も後方互換で可能（現在の登録: <b>${plainChannelCount} 件</b>）。ただし<b>フォーラムが正路</b>。</li>
 </ul>

 <div class="foot">LUDIARS / Discutere — loopback :3100 ・ <a href="/">トップへ戻る</a></div>
</div></body></html>`;

  guideRoutes.get("/guide", (c) => c.html(html));

  return guideRoutes;
}
