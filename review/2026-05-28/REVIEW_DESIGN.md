# 設計レビュー — Discutere (2026-05-28)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Discutere |
| 対象ブランチ / PR | main (feat/discatier-phase-3-translation-bridge 含む調査) |
| レビュー実施日 | 2026-05-28 |
| 対象コミット範囲 | 2b88810 〜 786a736 |

---

## 1. 設計強度 (Design Robustness)

| 評価 | 観点 | 所見 |
|------|------|------|
| B | 障害分離 | Haiku 呼び出し失敗時にルールベースへフォールバック (src/machina/haiku-classifier.ts:38)。DB 障害時の縮退は未定義 |
| C | 冪等性 | Webhook は handleSlackMessage を非同期で fire-and-forget (src/machina/routes.ts:765)。同一メッセージが二重送信された場合のべき等ガードがなく unique_chat_msg DB 制約でサイレント衝突 |
| B | 入力バリデーション | platform/mode/priority は定数配列チェックあり (routes.ts:473,479)。ただし title 長さ上限なし、dueDate 書式不検証 |
| C | エラーハンドリング | routes.ts 全ハンドラで try/catch が無く、DB 例外がそのまま Hono に伝播して 500 になる。Haiku のみ catch あり |
| C | リトライ・タイムアウト設計 | Haiku 呼び出し (haiku-classifier.ts:77) と GitHub Discussions 書き込みにタイムアウト設定なし。外部 API hang でリクエスト詰まりが発生しうる |
| B | 状態管理の明確性 | task/discussion セッションのオンメモリストア (mode-state.ts) の責務は明確。再起動消失の前提もドキュメント化済み |

### チェック項目

- [x] 単一障害点: Cernere 停止で認証全断 (設計上の前提として許容)
- [ ] 外部サービス障害時の縮退動作: Haiku は実装済、DB / GitHub Discussions は未定義
- [x] 入力境界値・型防御: 主要 Enum は定数チェックあり
- [ ] fail-safe: routes の DB 例外は try/catch 欠如
- [ ] 非同期タイムアウト: Haiku / GitHub API ともに AbortSignal 未設定
- [ ] race condition: taskSessionStore.update が Map 走査で concurrent request に対し非 atomic

---

## 2. 設計思想の一貫性 (Design Philosophy Compliance)

| 該当箇所 | 逸脱内容 | 本来の設計思想 | 推奨修正 |
|----------|---------|--------------|---------|
| src/machina/routes.ts:72-79 | checkGroupAccess が空配列を返し admin のみ通過 | workspace memberships は Cernere project-token claim から取得 | Cernere /api/auth/project-token か JWT claim を参照して実装 |
| src/machina/webhook-handler.ts:155-165 | completion 検出が mode=none 判定前に実行 | none: 何もしない (ログ保存のみ) (README:23) | mode チェックを completion 検出より前に置く |
| src/middleware/auth.ts:62-69 | 開発環境で X-User-Id ヘッダー fallback が有効 | Cernere Composite を唯一の認証経路とする方針 | dev フラグを feature flag 管理し、CI では強制無効化 |
| env-cli.config.ts:26 | JWT_SECRET の default が discutere-dev-secret-change-in-production | per-user / memory-only シークレット方針 | production ガードは required.production に入っているが、dev 環境でも warning を出すべき |

### チェック項目

- [x] レイヤー間の依存方向: routes -> machina -> db の順で適切
- [ ] 命名規則: logMachina が dead wrapper として残存 (routes.ts:57-59)
- [x] 共通パターン (リポジトリパターン): repository.ts に集約
- [ ] 設定値ハードコード: jwt 期限 900s が auth/routes.ts:19 と composite.ts:23 で重複定義

---

## 3. モジュール分割度 / 機能的凝集度

| モジュール | 凝集度評価 | 所見 |
|-----------|-----------|------|
| src/machina/routes.ts (1044行) | 通信的 | Routes + Business Logic + Session 操作が混在。前回から行数増加 |
| src/machina/webhook-handler.ts | 機能的 | Slack/Discord 受信正規化・モード分岐が整理されている |
| src/machina/mode-state.ts | 機能的 | セッションストアとして独立した責務を持つ |
| src/machina/haiku-classifier.ts | 機能的 | LLM/ルールのデュアル実装が明確に分離 |
| src/ludus/ | 機能的 | 辞書学習ドメインとして独立。machina との結合は repository 経由 |

### チェック項目

- [ ] SRP: routes.ts がルーティング + ビジネスロジック + セッション操作を担う (God Route)
- [x] God Object なし: machina 内の責務は分散されている
- [ ] N+1: routes.ts:101-105 でユーザー名解決を userIds の数だけループクエリ
- [x] 循環依存なし

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | 設計強度 | C | 2 |
| 2 | 設計思想の一貫性 | C | 1 |
| 3 | モジュール分割度 | B | 1 |
