# REVIEW — Discutere (2026-05-28)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Discutere (Di) |
| スタイル | Web サービス |
| 対象ブランチ / PR | main (PR #10/#11/#12/#13 merged, PR #13 storage-core + phase-3 branch 調査含む) |
| レビュー実施日 | 2026-05-28 |
| 対象コミット範囲 | 2b88810 〜 786a736 |
| 前回レビュー | 2026-05-13 (weighted_score C+/65) |

---

## 総合評価 (Overall Assessment)

| # | レビュー観点 | 区分 | 評価 | 重大指摘数 | ドキュメント |
|---|------------|------|------|-----------|------------|
| 1 | 設計強度 | 共通 | C | 2 | [REVIEW_DESIGN.md](REVIEW_DESIGN.md) |
| 2 | 設計思想の一貫性 | 共通 | C | 1 | [REVIEW_DESIGN.md](REVIEW_DESIGN.md) |
| 3 | モジュール分割度 | 共通 | B | 1 | [REVIEW_DESIGN.md](REVIEW_DESIGN.md) |
| 4 | コード品質 | 共通 | C | 1 | [REVIEW_IMPLEMENTATION.md](REVIEW_IMPLEMENTATION.md) |
| 5 | コードレベル脆弱性 | 共通 | C | 2 | [REVIEW_VULNERABILITY.md](REVIEW_VULNERABILITY.md) |
| 6 | テスト戦略・カバレッジ | 共通 | D | 2 | [REVIEW_QUALITY.md](REVIEW_QUALITY.md) |
| 7 | ライセンス遵守 | 共通 | A | 0 | [REVIEW_QUALITY.md](REVIEW_QUALITY.md) |
| 8 | ドキュメント完備性 | 共通 | B | 1 | [REVIEW_QUALITY.md](REVIEW_QUALITY.md) |
| 9 | 機能改善 | 共通 | — | — | [REVIEW_MISSING_FEATURES.md](REVIEW_MISSING_FEATURES.md) |
| 10 | 不足機能 | 共通 | — | — | [REVIEW_MISSING_FEATURES.md](REVIEW_MISSING_FEATURES.md) |
| 11 | Web 脆弱性 | Web | C | 1 | [REVIEW_VULNERABILITY.md](REVIEW_VULNERABILITY.md) |
| 12 | ゼロトラスト | Web | D | 1 | [REVIEW_VULNERABILITY.md](REVIEW_VULNERABILITY.md) |
| 13 | セキュリティ強度 | Web | C | 1 | [REVIEW_VULNERABILITY.md](REVIEW_VULNERABILITY.md) |
| 14 | データスキーマ | Web | C | 2 | [REVIEW_IMPLEMENTATION.md](REVIEW_IMPLEMENTATION.md) |
| 15 | SRE | Web | C | 2 | [REVIEW_IMPLEMENTATION.md](REVIEW_IMPLEMENTATION.md) |
| 16 | パフォーマンス・ベンチマーク | Web | C | 0 | [REVIEW_QUALITY.md](REVIEW_QUALITY.md) |
| 17 | クロスプラットフォーム互換 | Web | B | 0 | [REVIEW_QUALITY.md](REVIEW_QUALITY.md) |

**加重スコア: C (前回 C+/65 から横ばい〜微悪化)**

- A: 1 項目 (ライセンス)
- B: 4 項目 (モジュール分割度・ドキュメント・コード品質の一部・クロスプラットフォーム)
- C: 10 項目
- D: 2 項目 (テスト・ゼロトラスト)

---

## Critical / High 指摘の概要

### 1. Webhook 署名検証の完全欠落 [Critical]

`src/machina/routes.ts:747,778` — Slack/Discord の両 Webhook エンドポイントに認証も署名検証も存在しない。`botSigningSecret` は DB (`src/db/schema.ts:43`) に保存されているが受信時に使用されていない。URL を知る第三者が任意の `workspaceId` でメッセージを偽装でき、Haiku API の課金消費・タスク大量生成・BOT スパムが可能。

### 2. checkGroupAccess スタブによるアクセス制御の完全不能 [High]

`src/machina/routes.ts:72-79` — 前回から未修正。`admin` 以外のユーザーは空配列判定で全 workspace 403。admin は全 workspace への全アクセスが可能という二択。Cernere の workspace membership 経路の実装が必要。

### 3. JWT_SECRET のデフォルト平文 [High]

`env-cli.config.ts:26` — `"discutere-dev-secret-change-in-production"` がデフォルト値として Infisical に初期書き込みされる。`production` 必須ガードはあるが、dev 環境でこの値がそのまま使われた場合に service_token が偽造される。起動時の warn ログまたは非 production でも随時警告が必要。

### 4. テスト・CI の完全不在 [High]

`.github/workflows/` が存在しない。`tests/core/` は Discatier (Kuzu グラフ) の core 層テストのみで、Machina (Hono) API / Webhook / Haiku 分類フローのテストが皆無。新規追加された Discatier phase-1-3 の事業コア機能もテスト未整備のまま main に入っている。

### 5. オンメモリセッションストアの無制限蓄積 [High]

`src/machina/mode-state.ts:89-90` — `taskSessions` / `discussionSessions` の Map に上限がない。長時間プロセスでメモリリークが進行する。加えてプロセス再起動でヒアリング中タスクが消失し、ユーザーへの通知もない。

---

## 前回 (2026-05-13) からの比較

### 改善された項目

- **Discord 正規化モジュール追加** (PR #12) — `src/discord-hook/normalize.ts` が実装され、Discord inbound の型安全な処理が整備された
- **Ludus 辞書学習** (PR #11) — `/ask-ai` コマンドと mechanics-learner が追加。サービスの差別化機能が具体化した
- **Discatier Event-Sourced Storage** (PR #13) — Kuzu グラフ + JSONL イベントログの core 層が実装され、`tests/core/` のテストも追加された
- **`env-cli.config.ts` の production required ガード** — `JWT_SECRET`, `CERNERE_PROJECT_CLIENT_SECRET` が production では必須化された

### 悪化・未解消の項目

- **Webhook 署名検証**: 前回から未修正。Critical のまま
- **checkGroupAccess スタブ**: 前回から未修正。High のまま
- **routes.ts の行数**: 980 行 → 1044 行に増加 (Ludus ルート追加)
- **テスト/CI**: Discatier core の tests は増えたが Machina API のテストは相変わらず皆無
- **CLAUDE.md**: 引き続き未作成

---

## 推奨優先順位

1. **Webhook 署名検証の実装** (`src/middleware/webhook-signature.ts` 新規) — Critical リスクの排除
2. **`checkGroupAccess` の実装** — Cernere JWT claim または project-token claim で workspace membership を取得
3. **`mode=none` の early return** — `webhook-handler.ts:155` より前に移動してメッセージ処理をスキップ
4. **CI (GitHub Actions) 追加** — `npm run test:core` 自動実行 + tsc ビルドチェック
5. **routes.ts のハンドラ分割** — `src/machina/handlers/` に機能別ファイル分割

詳細は各 `REVIEW_*.md` を参照。
