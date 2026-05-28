# 実装評価 — Discutere (2026-05-28)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Discutere |
| 対象ブランチ / PR | main (PR #10-#13 含む) |
| レビュー実施日 | 2026-05-28 |
| 対象コミット範囲 | 2b88810 〜 786a736 |

---

## 1. コード品質 (Code Quality)

| 評価 | 観点 | 所見 |
|------|------|------|
| C | ルーティング肥大化 | `src/machina/routes.ts` が 1044 行。前回 (980 行) より増加。Handler ごとに `userId チェック → role チェック → group アクセス → DB 操作` の 6 行パターンが 14 回重複 |
| B | 型安全性 | `c.set("userId" as never, ...)` の `as never` キャストが `src/middleware/auth.ts:36,37,53-55` に多数。Hono の型定義 (`Variables`) を使えば解消可能 |
| B | 命名・可読性 | `logMachina` は `console.log` の薄いラッパーで実質 dead code (`routes.ts:57-59`)。関数名が誤解を招く |
| C | エラー境界 | routes の全 DB 操作が try/catch なし。Drizzle の better-sqlite3 同期例外が Hono に素通りして 500。ユーザーに DB エラー詳細が露出する可能性 |
| B | 動的 import | `routes.ts:104` に `await import("../db/repository.js")` が埋め込まれている。モジュールキャッシュで実害は少ないが、静的 import に統一すべき |
| B | マジックナンバー | `mode-sessions` ルートの停滞判定閾値 `5 * 60_000` (`routes.ts:922`) が定数化されておらず、`discussionDelayMinutes` とも独立 |

---

## 2. データスキーマの妥当性 (Data Schema Validation)

| テーブル / モデル | 問題種別 | 説明 | 推奨対応 |
|-----------------|---------|------|---------|
| `tasks.due_date` | 型不整合 | `text("due_date")` で書式未定義。ISO 8601 / 日本語自然言語どちらでも受け付けてしまう | `date` 型 or ISO 8601 正規化を DB 保存前に実施 |
| `tasks.status`, `tasks.priority` | 制約不足 | Drizzle SQLite は CHECK 制約を自動生成しない。コード側の Enum チェックのみで DB レベルの制約なし | `drizzle-zod` or Drizzle の `.check()` で DB 制約追加 |
| `channel_monitors.bot_token` | データ保護 | プレーンテキスト保存。Slack xoxb-... トークンが SQLite ファイルに平文で残る | application-level 暗号化 (`AES-256-GCM`) or 外部シークレットストア |
| `task_logs.task_id` | 制約不足 | `references(() => tasks.id)` に `onDelete` が指定されておらず、タスク削除後もログが残る | `onDelete: "cascade"` を追加するか、論理削除に変更 |
| `chat_messages`, `chat_summaries` | 正規化 | `workspace_id` が `monitor_id` から導出可能な冗長カラム。JOIN コスト削減のための意図的非正規化なら注釈が欲しい | コメントで明示するか、クエリで JOIN に変更 |

### チェック項目

- [x] 正規化: 概ね適切 (workspace_id 冗長は既知)
- [ ] 制約: status/priority の DB レベル CHECK 制約なし
- [ ] 外部キー: task_logs の ON DELETE 未指定
- [x] インデックス: クエリパターンに対応したインデックスが schema.ts で定義済み
- [x] API ↔ DB 整合: 基本的に型推論が一致

---

## 3. SRE 観点 (SRE Review)

| 評価 | 観点 | 所見 |
|------|------|------|
| C | 可観測性 | ログは `console.error/log` のみ。構造化ログ (JSON)・リクエスト ID・トレース ID がなく、エラー原因の追跡が困難 |
| B | デプロイ安全性 | Drizzle `db:push` で schema を適用する運用。`db:migrate` (SQL ファイル管理) に切り替えればロールバック可能 |
| C | スケーラビリティ | オンメモリセッションストア (`mode-state.ts`) が単一プロセス前提。水平スケールでセッションが消失する |
| C | 障害復旧 | SQLite バックアップ手順が未定義。WAL モードは設定されているが、定期スナップショットなし |
| B | 依存関係管理 | `package-lock.json` あり。`bcryptjs` が実際には未使用の可能性 (`package.json:28` に依存があるが使用箇所未確認) |

### チェック項目

- [ ] 構造化ログ: 未実装
- [ ] メトリクス収集: 未実装
- [x] ヘルスチェック: `/health` エンドポイントあり (`src/index.ts:23`)
- [ ] ロールバック可能なデプロイ: `db:migrate` 未使用
- [ ] SLI/SLO 定義: 未定義
- [ ] バックアップ手順: 未整備

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | コード品質 | C | 1 |
| 2 | データスキーマ | C | 2 |
| 3 | SRE | C | 2 |
