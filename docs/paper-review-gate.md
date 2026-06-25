# ディスカッションペーパー 編集ゲート (md 正本 + Notion 風編集)

議論/改善フローを始める **直前** に、ディスカッションペーパー (議題ブリーフ) と
集めた情報を **人間が確認 → 編集 → 確定** してから議論を始めるゲート。
ペルソナが議論する「前提」を人手で正してから走らせることで、議論の精度を上げる。
Di 内の Web UI だけで Discord と同じ議論を回せる**正規フロー**の入口。

## 入口 (チャット議論) と 議論一覧

トップ (`/`) の **「チャット議論」** カード → `/flow`。`/flow` は **議論一覧がホーム**:

- 既定で **議論一覧** (`GET /api/flow/sessions`、開始済み discussion_paper を新しい順) を表示。
  進行中 (未収束) も収束済みも在庫として並ぶ (= 一度議論を投げたら保存され一覧に出る)。
- **「＋ 新規議論開始」** → 入力フォーム (テーマ + 議論タイプ=議論既定 + ターン数 + タグ) を表示。
- **送信すると即座にペーパー編集画面へ遷移** (草案生成中は「準備中…」) → 確定で議論が自動進行。
- 一覧の項目クリックで **その議論のライブ表示**を開く (進行中はライブ更新 / 収束済みは最終状態)。
  各画面の **「← 議論一覧へ」** で一覧に戻る。

確定すると議論が自動進行し、**ペーパー + 各 LLM の意見がライブ描画され、ペーパーが更新されていく**
(§議論ライブ表示)。(`/chat` は別物の「フリーチャット」= 軽量チャットでその場議論。)

**保存タイミング**: 議論は確定 (approve) 時に `runFlow` 冒頭の `persistPaper` で `discussion_paper`
行が作られ、発話は毎ターン `flow_utterance` に永続する。= **開始した時点で保存**され、収束前でも
一覧・ライブ表示・再訪で参照できる (収束は `flow_conclusion` の有無で表示)。

## ハイブリッド源泉モデル (本文 md を正本)

ペーパーの **本文 (議題 / 観点補足 / メカニクス) は markdown を正本** とする
(`src/flow/paper-markdown.ts`)。

- 各 LLM (ペルソナ) には `buildPaperSystem` がこの md を **そのまま system に載せる**
  (= 各 LLM が md を直接参照)。`bodyMd` が無い旧経路は構造化フィールドから従来どおり組み立てる
  (後方互換、出力はバイト等価)。
- 編集は md を **ブロック (見出し / 段落 / 箇条書き) 単位** で扱う (`src/flow/paper-blocks.ts`)。
- 構造化フィールド (mechanics 等) は md から **派生し直し** (`markdownToPaperDraft`)、
  `mechanics_json` / 機密 synthetic 等の付随機能に使う。観点タグ (機密/内部/運用/開発) は
  本文 md の外の操作フラグとして構造化のまま保持する。
- 永続: `discussion_paper.body_md` (正本 md) + `discussion_paper_revision` (編集の版履歴)。

## 設定

- `flow.paperReview.enabled` (既定 **false** = opt-in)。Discord スレッド返信での編集を含む全経路の有効化。
- `flow.paperReview.webCanonical` (既定 **true**)。Web UI (`/flow`) を **常に編集ゲート経由の正規フロー**
  にする。`enabled=false` でも Web の議論/改善は Notion 風編集を必ず通る (Discord は `enabled` に従う)。
  Web を従来の即時開始に戻すなら `false`。
- `flow.paperReview.model` はペーパー編集に使うモデル (空なら LLM の既定モデル)。
- `flow.paperReview.timeoutMs` は無操作の自動開始タイムアウト (既定 **0** = 無期限に承認を待つ。
  `>0` なら提示から `timeoutMs` 経過で草案のまま自動開始し、編集があるたび延長)。
- env: `DISCUTERE_FLOW_PAPER_REVIEW_ENABLED` / `_WEB_CANONICAL` / `_MODEL` / `_TIMEOUT_MS`。
- 対象フロー: `discussion` / `improvement`。`learning` / `sparring` は対象外。
- ゲートを通さない時 (Discord で `enabled=false`) は従来どおり情報ゲート後にそのまま議論を自動開始する。

## 流れ

```
フロー起動 → 情報ゲート (情報密度評価 + 不足観点クロール)
          → ペーパー草案を組み立て (investigate のメカニクス + 観点補足) → 本文 md 生成 (rev1)
          → 【編集ゲート】人間がブロック編集 / LLM レビュー / 根拠クロール / 戻す
          → 確定 (議論開始ボタン) → 確定ペーパー (bodyMd) で議論を開始 (investigate は再実行しない)
```

確定ペーパーは `runFlow(paperOverride)` に `bodyMd` 込みで渡され、`investigate` (step 1) を
**省略**してその本文 md / メカニクス / 観点補足で議論を回す (議題/タグは通常の引数で渡す)。

## 共有コア (`src/flow/paper-review.ts`)

トランスポート非依存。Discord / Web の両方が使う。

- `buildPaperDraft(theme, tags, deps)` — `investigateTheme` でメカニクスを取り、タグから観点補足を
  作り、本文 md (`withDerivedBody`) と集めた情報サマリ (件数 + サンプル) を付けた草案を返す (永続化しない)。
- `applyPaperEdit(draft, instruction, llm)` — 自然文の調整指示を LLM でペーパー全体に反映 (構造化編集
  → 本文 md 再生成)。検証に失敗したら **元のドラフトを保ち** `applied=false` を返す。
- `reviewBlock({bodyMd, blockId, instruction?, llm})` — 本文 md の 1 ブロックを LLM で **改稿提案**
  (適用しない=UI が diff 提示して採否)。ブロック不在/LLM 失敗は `ok=false` で original を返す。
- `gatherEvidence({topic, listExternalVoices, llm?})` — ブロックの根拠となる外部の声を RAG で集約し、
  挿入用の根拠段落を提案する (クロール補佐。KG 書込みはしない=単一writer衝突回避)。
- `coercePaperDraft(raw, fallback)` — Web の確定入力 (本文 md + tags) を `PaperDraft` に正規化
  (本文 md を最優先で正本化、不正タグ除去)。
- `withDerivedBody` / `withDerivedStructure` — 構造化 ⇄ 本文 md の相互派生。
- `renderPaperReview(draft, info)` — ペーパー + 情報サマリを人間向け markdown に (Discord 表示)。
- `isApprovalText(text)` — 「開始」「承認」「OK」「✅」等の承認語判定 (調整指示と区別)。

## ブロック / 版履歴 (`paper-blocks.ts` / `paper-revisions.ts`)

- `splitBlocks(md)` / `joinBlocks` / `replaceBlock` / `insertBlockAfter` / `getBlockText` —
  md ⇄ ブロック (見出し / 段落 / 箇条書き) の純関数。ブロック id は出現順 `b0`,`b1`,…。
- `appendRevision` / `latestRevision` / `listRevisions` / `canRevert` / `revertLast` —
  session 単位の版履歴 (追記専用)。「戻す」は 1 手前の本文を新リビジョンとして積み直す
  (前進専用=監査ログを失わない)。サーバ再起動を跨いでも編集途中のペーパーが残る。

## Discord (スレッド)

`src/flow/discord-live.ts` + `src/discord-hook/gateway.ts`。

1. フォーラム投稿で議論/改善が起動 → 情報ゲート後、`startPaperReview` が草案 + 情報サマリを
   スレッドに投稿し、`paperReviewByThread` にレビュー待ちで登録する。
2. スレッド返信を `handlePaperReviewReply` が処理 (壁打ちより優先):
   - 承認語 (「開始」等) → `handlePaperReviewApproval` → 確定ペーパーで `runDiscussionDispatch` (= 議論開始)。
   - それ以外 → `applyPaperEdit` で反映し、更新版ペーパーを再掲 (自動開始タイマーを延長)。
3. 承認後は従来どおり完走 → 収束まとめを投稿。

承認は **スレッド返信「開始」** または **✅ リアクション** で行う。✅ は `gateway` の
`MessageReactionAdd` がレビュー中スレッド (`hasPaperReview`) を検知して `handlePaperReviewApproval`
に回す (スコアリングより優先)。`timeoutMs>0` なら無操作で `timeoutMs` 経過後に草案のまま自動開始する。

## Web (`/flow`) — Notion 風ブロックエディタ (正規フロー)

`src/flow/web/routes.ts` + `src/flow/web/page.ts`。`webCanonical=true` (既定) なら議論/改善は必ずこのゲートを通る。

- `POST /api/flow/start` — ゲート有効時は `{ ok, kind, sessionId, review: true }` を返し、
  バックグラウンドで情報整備 + 草案構築 → 初期本文 md を rev1 として記録 (`paperReviews` に格納)。
- `GET  /api/flow/:session/paper` — 草案の取得 (`ready` になるまでポーリング)。
  `{ paper(bodyMd 込み), blocks, canRevert }` を返す。
- `POST /api/flow/:session/paper/block/review` `{ blockId, instruction? }` — 1 ブロックを LLM 改稿提案
  (適用しない)。`{ reviewed, original, proposed, rationale }` を返し、UI が old/new diff を提示。
- `POST /api/flow/:session/paper/block/apply` `{ blockId, newText, summary? }` — 改稿採用 / 手編集 /
  削除 (newText 空) を本文に適用し版履歴に追記。
- `POST /api/flow/:session/paper/crawl` `{ blockId?, query?, insert? }` — ブロックの根拠を RAG で集約。
  `insert:true` で提案段落を対象ブロック直後に挿入して確定。
- `POST /api/flow/:session/paper/edit` `{ instruction }` — 自然文の全体調整 (本文 md 再生成)。
- `POST /api/flow/:session/paper/revert` — 1 手前の本文に戻す (版履歴 revert)。
- `POST /api/flow/:session/paper/approve` `{ paper:{ bodyMd, tags } }` — 確定して議論開始。

ブラウザは本文 md をブロックカードで並べ、ブロックごとに **保存 / LLMレビュー / 根拠を集める / 削除**
を行う。LLM レビューは old(赤)/new(緑) の diff を提示し **採用 / 却下**。**「↶ 戻す」** で 1 手前に戻る。
**「ペーパーを確定する」** にチェックして初めて **「議論開始」** ボタンが活性化し、押すと確定ペーパー
(bodyMd) で議論を開始 → 議論ライブ表示に切り替わる。`timeoutMs>0` なら無操作で自動開始
(編集があるたび延長)。

## 議論ライブ表示 (ペーパーが更新されていく)

確定後は議論が **自動進行** し、`/flow` が **2 ペイン** のライブ表示になる:

- **左: ディスカッションペーパー** — `GET /api/flow/:session/status` の `paperMd` を markdown 描画。
  議論の進行に合わせて **更新されていく** (変化時に黄色フラッシュ + 更新時刻)。
- **右: 各 LLM の意見** — `status.utterances` を発話順に逐次追記 (1.5s ポーリング)。

ペーパーの更新は director がラウンドごとに **base ブリーフ + 議論の経過 (各ラウンドの まとめ / 止揚) +
結論** を `renderProgressMarkdown` で焼き直し、`updatePaperBody` で `discussion_paper.body_md` を上書き
することで成立する (LLM に渡す system は base ブリーフのまま=キャッシュ安定。表示/永続の body_md だけが
育つ)。`# 議論の経過` 節は毎回 `stripProgress` で土台化してから足し直すので冪等。

## 制約 / follow-up

- 自動開始タイムアウトは `timeoutMs` で設定可 (既定 0 = 無期限に待つ)。Discord/Web ともサーバ側タイマー。
- 編集/確定は誰でも可 (匿名 workspace 方針。admin ゲートは設けない)。
- クロール補佐は **収集済み KG の RAG 参照のみ** (編集中の KG 書込みはしない=単一writer衝突回避)。
  新規ソースの実クロールは議論開始前の情報ゲートが担う。
- 自由編集で本文 md からメカニクス見出しを消すと構造化 mechanics は fallback を保つ (付随機能の取りこぼし防止)。
- Discord は従来の構造化編集 (スレッド返信) のまま (ブロック編集は Web 専用)。本文 md は両経路で正本化される。
- 個人データは扱わない (情報サマリ/根拠は出所付き・個人仮名のまま)。
