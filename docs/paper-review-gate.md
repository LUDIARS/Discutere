# ディスカッションペーパー レビューゲート

議論/改善フローを始める **直前** に、ディスカッションペーパー (議題ブリーフ) と
集めた情報を **人間が確認 → 自然文で調整 → 承認** してから議論を始めるゲート。
ペルソナが議論する「前提」を人手で正してから走らせることで、議論の精度を上げる。

- 設定: `flow.paperReview.enabled` (既定 **false** = opt-in)。`enabled=true` で有効化。
  `flow.paperReview.model` はペーパー編集に使うモデル (空なら LLM の既定モデル)。
  `flow.paperReview.timeoutMs` は無操作の自動開始タイムアウト (既定 **0** = 無期限に承認を待つ。
  `>0` なら提示から `timeoutMs` 経過で草案のまま自動開始し、調整があるたび延長)。
  env: `DISCUTERE_FLOW_PAPER_REVIEW_ENABLED` / `_MODEL` / `_TIMEOUT_MS`。
- 対象フロー: `discussion` / `improvement`。`learning` / `sparring` は対象外。
- 無効時は従来どおり、情報ゲート (クロール) のあとそのまま議論を自動開始する。

## 流れ

```
フロー起動 → 情報ゲート (情報密度評価 + 不足観点クロール)
          → ペーパー草案を組み立て (investigate のメカニクス + 観点補足 + 集めた情報サマリ)
          → 【レビューゲート】人間が確認・調整・承認
          → 確定ペーパーで議論を開始 (investigate は再実行しない)
```

確定ペーパーは `runFlow(paperOverride)` に渡され、`investigate` (step 1) を **省略**して
そのメカニクス/観点補足で議論を回す (議題/タグは通常の引数で渡す)。

## 共有コア (`src/flow/paper-review.ts`)

トランスポート非依存。Discord / Web の両方が使う。

- `buildPaperDraft(theme, tags, deps)` — `investigateTheme` でメカニクスを取り、タグから観点補足を
  作り、`listExternalVoices` で集めた情報サマリ (件数 + サンプル) を付けた草案を返す (永続化しない)。
- `applyPaperEdit(draft, instruction, llm)` — 自然文の調整指示を LLM でペーパーに反映 (構造化編集)。
  JSON で全文を返させ、検証に失敗したら **元のドラフトを保ち** `applied=false` を返す (ブリーフを壊さない)。
- `coercePaperDraft(raw, fallback)` — Web フォームの直接編集 JSON を `PaperDraft` に正規化 (不正タグ除去)。
- `renderPaperReview(draft, info)` — ペーパー + 情報サマリを人間向け markdown に。
- `isApprovalText(text)` — 「開始」「承認」「OK」「✅」等の承認語判定 (調整指示と区別)。

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

## Web (`/flow`)

`src/flow/web/routes.ts` + `src/flow/web/page.ts`。

- `POST /api/flow/start` — `paperReview.enabled` 時は `{ ok, kind, sessionId, review: true }` を返し、
  バックグラウンドで情報整備 + 草案構築 (`paperReviews` に格納)。
- `GET  /api/flow/:session/paper` — 草案の取得 (`ready` になるまでブラウザがポーリング)。
- `POST /api/flow/:session/paper/edit` `{ instruction }` — 自然文調整を反映して更新後ペーパーを返す。
- `POST /api/flow/:session/paper/approve` `{ paper? }` — 承認して議論開始。`paper` があれば
  フォームの直接編集 (議題/タグ/観点補足/メカニクス) を確定値に採用する。

ブラウザは確認フォームで議題・タグ・観点補足・メカニクス (1 行 1 件 `名前 :: 説明 :: 期待感情`) を
直接編集でき、自然文の「調整を反映」も併用できる。「この内容で開始」で承認→議論開始→通常の
ポーリング表示に切り替わる。`timeoutMs>0` なら無操作で経過後にサーバ側タイマーが草案のまま自動開始する
(調整があるたび延長)。

## 制約 / follow-up

- 自動開始タイムアウトは `timeoutMs` で設定可 (既定 0 = 無期限に待つ)。Discord/Web ともサーバ側タイマー。
- 承認/調整は誰でも可 (匿名 workspace 方針。admin ゲートは設けない)。
- 承認は Discord = テキスト「開始」 or ✅ リアクション、Web = フォームのボタン。
- 個人データは扱わない (情報サマリは出所付き・個人仮名のまま)。
