# 不足機能評価 — Discutere (2026-05-28)

| 項目 | 値 |
|------|-----|
| リポジトリ | LUDIARS/Discutere |
| 対象ブランチ / PR | main (PR #10-#13 含む) |
| レビュー実施日 | 2026-05-28 |
| 対象コミット範囲 | 2b88810 〜 786a736 |

---

## 1. 機能の改善提案 (Feature Improvement)

| 対象機能 | 改善提案 | 期待効果 | 優先度 |
|---------|---------|---------|--------|
| `checkGroupAccess` (`routes.ts:72-79`) | Cernere `/api/auth/project-token` の claim か JWT ペイロードに workspace membership を含め、スタブを実装に置き換える | 非 admin ユーザーが実際に workspace 操作できるようになる | High |
| Webhook 受信 (`routes.ts:747-797`) | Slack HMAC-SHA256 / Discord Ed25519 の署名検証を追加 | 第三者による偽メッセージ投入を防止 | High |
| タスク完了検出 (`webhook-handler.ts:155-165`) | `mode=none` の早期 return を completion 検出より前に移動、かつ completion は同チャンネルのタスクに限定 | README との矛盾解消、誤 close 防止 | High |
| `resolveAssignee` (`webhook-handler.ts:242-263`) | Cernere workspace メンバー一覧を取得して名前解決。現在は空配列で実質 dead code | タスク自動アサインが実際に機能するようになる | Medium |
| JWT 有効期限 (`composite.ts:23`, `auth/routes.ts:19`) | Refresh token 経路の実装または Cookie 有効期限延長 (現在 15 分)。セッション中に期限切れで強制ログアウトが起きる | UX 改善 | Medium |
| タスク一覧の N+1 (`routes.ts:101-105`) | `userRepo.findByIds(userIds)` のバルク取得に変更 | アサイニーが多い場合のレスポンス改善 | Low |

---

## 2. 不足機能の提案 (Missing Feature Proposal)

| 提案機能 | 必要性の根拠 | 実装優先度 | 想定影響範囲 |
|---------|------------|-----------|------------|
| Webhook 署名検証ミドルウェア | 現在は外部から任意投入が可能。BOT トークン・Haiku 課金リスク | High | `src/machina/routes.ts`, 新規 `src/middleware/webhook-signature.ts` |
| zod による入力 schema validation | `c.req.json<{...}>()` の型キャストのみで境界値・書式が未検証。タイトル 0 文字、不正 dueDate 文字列を受け入れる | High | 全 POST/PUT handler |
| 構造化ログ (pino 等) | `console.error` のみでは本番運用でのトレースが困難。Discatier phase-3 の translation bridge はさらに複雑になる | High | `src/index.ts`, 全モジュール |
| CI パイプライン (GitHub Actions) | 現在テストが `npm run test:core` を手動実行する形のみ。自動化なし | Medium | `.github/workflows/` |
| レート制限 | `/api/webhook/*` は認証なし・レート無制限。大量偽メッセージで Haiku 課金爆発 | Medium | `src/index.ts` or webhook handler |
| セッション永続化 (task/discussion) | オンメモリセッションはプロセス再起動で消滅。ヒアリング中タスクが復元できない | Medium | `src/machina/mode-state.ts` |
| CLAUDE.md の追加 | Claude Code による開発ルール・起動手順・禁止事項が未整備。他 LUDIARS リポは整備済み | Low | リポルート |
| Discatier core の統合テスト | `tests/core/` は Discatier (Kuzu グラフ) 用の別設計 (PR #13 系)。Machina (Hono) の API テストが不在 | Low | `tests/machina/` |

---

## 総合評価

| # | レビュー観点 | 指摘数 | 優先度別内訳 |
|---|------------|--------|------------|
| 1 | 機能改善 | 6 | High: 3 / Medium: 2 / Low: 1 |
| 2 | 不足機能 | 8 | High: 3 / Medium: 3 / Low: 2 |
