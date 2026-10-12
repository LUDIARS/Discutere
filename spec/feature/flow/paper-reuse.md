# ディスカッションペーパーの使い回し (2026-10-13)

既存のディスカッションペーパーのメカニクスと声を、別の議論でそのまま使えるようにする
(neco 指示 2026-10-13「ディスカッションペーパーのメカニクスと声を使いまわせるようにする」)。

## 何を引き継ぐか

| 材料 | 出所 | 新しいペーパーでの扱い |
|---|---|---|
| メカニクス | 元ペーパーの `discussion_paper.mechanics_json` | 草案のメカニクスの先頭に差す (`buildPaperDraft` の `extraMechanics`。重複は名前で除く) |
| ゲーム内容 | 元ペーパーの本文 md の固定欄 (`paperFixedFieldsFromMarkdown(...).mechanicsContext`、経過節は除く) | 今回の資料 (Notion 等) の前に連結して `mechanicsContext` にする (同じ内容は重ねない) |
| 声 | 元ペーパーに保存した声 (`discussion_paper_voice`) | その声だけで草案・議論を行い、**集め直さない** (ユーザーの声の収集・情報ゲート・自動クロールを省く) |

元ペーパーに保存した声が無い (この機能より前のペーパー) ときは、メカニクスとゲーム内容だけ引き継ぎ、声は通常どおり集める。

## 指定のしかた

- Discord: フォーラムのスレッド名か starter 本文に `ペーパー#12` (`ペーパー 12` / `paper#12` / 全角数字も可)。
  スレッドに「♻️ ペーパー #12「…」のメカニクス n 件・ゲーム内容・声 m 件 (集め直しません) を使い回します。」と知らせる。
  番号が無ければ「見つからないため、通常どおり材料を集めます」と知らせて通常の準備に戻る。
- Web (`/flow`): 開始フォームの「使い回すペーパー番号」欄 (`reusePaperNo`)。欄が空なら本文中の `ペーパー#N` を読む。
- 番号は議論一覧の議論番号 (`discussion_paper.discussion_no`)。下書きも対象。

## 声の保存 (`src/flow/paper-voices.ts`)

- テーブル `discussion_paper_voice (session_id, ordinal, content, source, source_url, created_at)`
  (migration `flow_0030_paper_voices`)。本文・出所・元 URL だけを持ち、個人アンカーは持たない。
- 保存の契機: ペーパー草案を作ったとき、と議論を開始するとき (レビュー中の取り込みで増えた声も含めて置き換える)。
  議題で引ける声を最大 `PAPER_VOICE_LIMIT` (200) 件。空の保存は既存を消さない。失敗は議論を止めない。
- 保存した声の検索 (`storedVoiceLookup`): 検索語 (`extractKeyTerms`) を多く含む声を先に、同点は保存順。
  一致しなくても保存した声を返す (使い回した声を必ず議論に渡す)。
- Discord の議論開始で、そのスレッドのメモリ上の声の検索が無い (再起動後のやり直し等) ときは、保存した声で議論する。
  Web の承認は、使い回した声 > そのセッションに保存済みの声 > 外部の声検索 の順で使う。

## 実装

- `src/flow/paper-reuse.ts`: `parseReusePaperNo` / `loadReusablePaper` / `mergeReusedContext` / `describeReuse`
- `src/flow/paper-voices.ts`: `savePaperVoices` / `loadPaperVoices` / `storedVoiceLookup` / `sessionVoiceLookup`
- 配線: `src/flow/discord-live.ts` (`resolveReusedPaper` / `saveThreadVoices` / 議論開始時の復元) と
  `src/flow/web/routes.ts` (`/api/flow/start` の `reusePaperNo`、`startApprovedFlow`)、`src/flow/web/page.ts` (入力欄)
- テスト: `tests/flow/paper-reuse.test.ts`
