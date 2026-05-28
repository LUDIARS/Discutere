# 品質保証レビュー — Discutere (2026-05-28)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Discutere |
| 対象ブランチ / PR | main (PR #10-#13 含む) |
| レビュー実施日 | 2026-05-28 |
| 対象コミット範囲 | 2b88810 〜 786a736 |

---

## 1. テスト戦略・カバレッジ (Test Strategy & Coverage)

| 評価 | 観点 | 所見 |
|------|------|------|
| C | unit テストの網羅性 | `tests/core/run.ts` は Discatier (Kuzu グラフ) の Event Store / Vector 検索を検証する独立テスト。Machina (Hono API) 層のテストは存在しない |
| C | integration テストの網羅性 | `tests/core/projection/run.ts`, `tests/core/bridge/translation/run.ts` が追加されたが、これも Discatier core のみ。Webhook 受信・Haiku 分類・タスク登録フローの統合テストは未実装 |
| D | E2E テスト | 皆無。Playwright / Cypress のセットアップなし |
| C | エッジケース・境界値 | `analyzer.ts` の正規表現・優先度判定・dueDateHint 解析のエッジケーステストが皆無。日本語自然言語解析は境界値テストの価値が高い |
| D | CI 自動実行 | `.github/workflows/` が存在しない。`npm run test:core` は手動実行のみ |

### チェック項目

- [x] コアロジックに unit テストあり: Discatier core のみ
- [ ] DB/Network を含む integration テストあり: Machina 側なし
- [ ] E2E テストあり: なし
- [ ] CI で毎コミット green 必須: CI なし
- [ ] カバレッジ計測ツール: 未設定

---

## 2. ライセンス遵守・OSS 帰属表示 (License Compliance)

| 該当依存 | ライセンス | 配布形態 | 互換性評価 | 帰属表示状態 |
|---------|----------|---------|-----------|-------------|
| hono | MIT | dynamic | OK | 対応済 (package.json) |
| drizzle-orm | Apache-2.0 | dynamic | OK | 対応済 |
| better-sqlite3 | MIT | dynamic | OK | 対応済 |
| jsonwebtoken | MIT | dynamic | OK | 対応済 |
| bcryptjs | MIT | dynamic | OK | 対応済 (未使用の疑いあり) |
| @hono/node-server | MIT | dynamic | OK | 対応済 |

### チェック項目

- [x] プロジェクトライセンス明記: `LICENSE` (MIT) + `README.md` に明記
- [x] 依存パッケージのライセンス範囲: 全 MIT/Apache-2.0、問題なし
- [ ] NOTICE ファイル: 未作成 (動的リンクのみなので必須ではないが推奨)
- [x] プロプライエタリ依存: Infisical は SaaS 利用で配布バイナリに非混入

---

## 3. ドキュメント完備性 (Documentation Completeness)

| 評価 | 観点 | 所見 |
|------|------|------|
| A | README の網羅性 | 概要・前提・起動手順・API 一覧・認証フローが充実している |
| B | DESIGN / アーキテクチャ | `docs/discatier_implementation_plan.md` が Discatier (Kuzu) 設計書として詳細。Machina (Hono API) 側の設計書は `docs/discatier_implementation_plan.md` に混在し、2 設計の境界が不明瞭 |
| B | API リファレンス | README のエンドポイント表が主。`/api/status` レスポンスも読める。型定義は TypeScript から推論可能 |
| C | inline コメント | Machina モジュール (analyzer.ts / task-mode.ts 等) はコメントが充実。`discussion-mode.ts` は文字化け (Shift-JIS 混在) している |
| D | CLAUDE.md | 未作成。他の LUDIARS リポ (Cernere / Memoria 等) は整備済み。CI・起動・開発ルールが未文書 |

### チェック項目

- [x] README: 整備済み
- [x] DESIGN.md 相当: `docs/` に存在
- [ ] CLAUDE.md: 未作成
- [ ] CHANGELOG: 未作成
- [ ] ランブック / トラブルシューティング: 未作成

---

## 4. パフォーマンス・ベンチマーク (Web 固有)

| 評価 | 観点 | 所見 |
|------|------|------|
| C | パフォーマンス要件の明文化 | Haiku 判定の許容レイテンシ・タスク一覧取得の目標応答時間が未定義 |
| D | ベンチマーク・負荷試験 | 存在しない |
| C | 高負荷時の挙動 | メモリ内セッションストアが無制限に蓄積。プロセス長時間動作でメモリリークの可能性 |

---

## 5. クロスプラットフォーム互換 (Web 固有)

| 評価 | 観点 | 所見 |
|------|------|------|
| B | サーバランタイム | Node.js 22+ を `package.json` で明記。`tsx` 経由で TypeScript 直接実行、本番は `tsc` ビルド後 `node dist/` |
| B | ブラウザ互換 | React 19 + Vite 8。対象ブラウザは `README` に明記なし |
| B | 文字エンコーディング | `discussion-mode.ts` に Shift-JIS コメント混入 (表示環境依存)。ソースとしては UTF-8 が基本 |
| C | コンテナ・再現性 | `Dockerfile` なし。デプロイ手順も未定義 |

---

## 総合評価

| # | レビュー観点 | 評価 | 重大指摘数 |
|---|------------|------|-----------|
| 1 | テスト戦略・カバレッジ | D | 2 |
| 2 | ライセンス遵守 | A | 0 |
| 3 | ドキュメント完備性 | B | 1 |
| 4 | パフォーマンス・ベンチマーク | C | 0 |
| 5 | クロスプラットフォーム互換 | B | 0 |
