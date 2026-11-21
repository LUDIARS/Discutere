# AI 試遊感想（SPEC-DI-AI-PLAYTEST-REVIEWS）

Actio `41a3f2dc-6a26-40a3-914a-1f17526e307c`、`3259cd8c-747c-453a-b63b-435c12c5a363`。

試遊連携の C++ 実装と回帰テストは `praesidium.json` に登録する。CMake ビルドとセットアップ前の Praesidium は、同じ追跡済み Augur 契約ランタイムヘッダーを参照する。
試遊契約ランタイムは Discutere の共通環境変数リーダーを使い、`VESTIGIUM_LOGS_DIR` に指定した UTF-8 パスへ観測ログを書き込む（PR #4003、Actio `37ee6778-714c-4851-85dd-64da25da66a5`）。

`POST /api/persona-bridge/ai-playtest-reviews` は既存 `persona-bridge:write` サービス Bearer または固定トークンで認証する。`Idempotency-Key` は `run_id` と完全一致する。

必須項目は `origin: "ai"`、`source: "ai-autoplayer"`、`run_id`、`game_id`、`game_title`、`summary`、`good`、`bad`、`confusions`、`reflection`、`played_at`。`good` と `bad` は各 1〜8 件、`confusions` は 0〜8 件。各項目は `aspect`（`ux` / `control` / `game`）、`text`、`evidence`（`ticks` / `shots` の非負安全整数配列、各最大 100 件）。`reflection` は 1〜8 行。`played_at` は Voluptas が正規化した UTC ISO 日時。未知の項目や形式違反は 400。

`ai_playtest_review` に初回の内容を保存し、初回は 201、再送は 200 で `{"ok":true,"run_id":"…"}` を返す。AI 感想は人の感想、声の RAG インデックス、ペルソナ統計に取り込まない。ゲームタイトルまたは ID に一致するレビューを議論ペーパーとプロンプトの専用欄に表示する。機密・内部タグでは外部の感想を参照しない。
