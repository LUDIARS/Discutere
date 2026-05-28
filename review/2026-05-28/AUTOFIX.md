# AUTOFIX — Discutere (2026-05-28)

| 項目 | 値 |
|------|-----|
| 実施日 | 2026-05-28 |
| 対象 | 実装フェーズ前のため自動修正なし |

---

## 自動修正対象: なし

Discutere は現在 `feat/discatier-phase-3-translation-bridge` ブランチで積極的に開発中であり、  
コードへの自動修正はコンフリクトリスクがあるため **今回は実施しない方針** とする。

---

## フラグした指摘 (手作業対応に回した Critical / High)

以下は REVIEW 系ドキュメントで指摘したが、自動修正ではなく開発者の判断が必要なもの。

- **[Critical] Webhook 署名検証の欠落**
  - 該当: `src/machina/routes.ts:747-774`
  - 詳細: `REVIEW_VULNERABILITY.md` §1, §2
  - 対応方針: `src/middleware/webhook-signature.ts` として Slack HMAC-SHA256 / Discord Ed25519 の検証 middleware を新規実装し、各 webhook ルートに適用する

- **[High] `checkGroupAccess` スタブの未実装**
  - 該当: `src/machina/routes.ts:72-79`
  - 詳細: `REVIEW_DESIGN.md` §2, `REVIEW_VULNERABILITY.md` §3
  - 対応方針: Cernere `/api/auth/project-token` または JWT payload の workspace claim を参照して実装

- **[High] JWT_SECRET デフォルト平文の警告不足**
  - 該当: `env-cli.config.ts:26`
  - 詳細: `REVIEW_VULNERABILITY.md` §1
  - 対応方針: `src/index.ts` 起動時に `JWT_SECRET` がデフォルト値のままなら WARN ログを出力。production の場合は起動拒否

- **[High] テスト・CI の不在**
  - 該当: `.github/workflows/` 不在
  - 詳細: `REVIEW_QUALITY.md` §1
  - 対応方針: GitHub Actions で `npm run test:core` + `npm run build` の CI を追加

- **[High] オンメモリセッションストアの無制限蓄積**
  - 該当: `src/machina/mode-state.ts:89-90`
  - 詳細: `REVIEW_IMPLEMENTATION.md` §3
  - 対応方針: セッション数の上限設定 (例: 1000 件) または TTL による自動削除を追加

- **[Medium] `mode=none` でも completion 検出が実行される**
  - 該当: `src/machina/webhook-handler.ts:155-165`
  - 詳細: `REVIEW_DESIGN.md` §2
  - 対応方針: `mode` チェックを `processMessage` の上部に移動する (安全な 1 行変更だが影響確認が必要)
