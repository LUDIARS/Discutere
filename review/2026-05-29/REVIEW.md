# REVIEW — Discutere (2026-05-29)

総合評価: **B-**

前回 (2026-05-13) **B** → **B-** へ低下。 主因は、 10 commit 積み重ねの中で **Discatier Core + Phase 5/6 仮説ライフサイクル** が大幅に追加された一方で、 **既存の MACHINA webhook レイヤー脆弱性が未修正のまま**、 かつ **Discord pivot 実装が未完了** であること。

## 総合評価

| # | 観点 | 評価 | 重大指摘 |
|---|------|------|---------|
| 1 | 脆弱性 (Web) | C | 2 Critical (webhook 署名検証欠落 Slack/Discord) |
| 2 | 設計強度 | B+ | 0 |
| 3 | 設計思想の一貫性 | B+ | 0 |
| 4 | モジュール分割度 | B | 1 (routes.ts 大型化) |
| 5 | コード品質 | C | 2 (resolveAssignee TODO, race condition) |
| 6 | データスキーマ | B | 1 (Discatier ↔ MACHINA 二重 session state) |
| 7 | 機能改善 | C | 2 |
| 8 | 不足機能 | C | 3 (webhook検証/checkGroupAccess/resolveAssignee) |
| 9 | SRE | C | 2 (N+1 query, completion 二重発火) |
| 10 | ゼロトラスト | C | 1 (JWT_SECRET default) |
| 11 | セキュリティ | C | 2 (webhook 署名欠落) |
| 12 | テスト戦略 | C | 1 (MACHINA layer test なし, phase2 failing main 統合済) |
| 13 | パフォーマンス | C | 2 (N+1 query, race condition) |
| 14 | ライセンス | B | 1 (bcryptjs dead dep) |
| 15 | クロスプラットフォーム | B | 0 |
| 16 | ドキュメント完備性 | B | 2 (CLAUDE.md なし, PLAN.md outdated) |

| 観点 | 評価 | サマリ |
|------|------|--------|
| DESIGN | B+ | Discatier Core (Phase 1~6) の Event Sourcing + projection アーキテクチャは堅牢 |
| VULNERABILITY | C | Webhook 署名検証ゼロ (前回と同じ)、 JWT_SECRET default value ハードコード継続、 Discord Interactions endpoint は Ed25519 検証を実装したが routes.ts で未利用 |
| IMPLEMENTATION | C | `resolveAssignee` TODO 放置拡大、 N+1 query 改善なし、 race condition が Discatier 統合で複雑化、 completion 二重発火、 phase2 synthesis-handlers.test.ts failing で main merge |
| MISSING | C | Slack/Discord webhook signature 検証実装ゼロ、 CLAUDE.md なし、 Discatier 永続化 (Di-9) 未実装 |
| QUALITY | B | Discatier core 側のテストは充実 (24 test files) したが、 MACHINA routes.ts は相変わらず 980 行単一ファイル |

## 主な所見

### A. 新規設計層（強み）

1. **Discatier Core の Event Sourcing が堅牢**: `src/core/events/event-log.ts` + `src/core/events/projection.ts` で phase 1~6 の immutable event trail を確立
2. **Vector + Embedding パイプラインの下地**: `src/core/vectors/embedding.ts` / `src/core/vectors/vector-search.ts` で similarity search の型整備

### B. 脆弱性（前回からの悪化）

1. **Webhook 署名検証が完全欠落**:
   - `src/machina/routes.ts:749-778` (Slack) は X-Slack-Signature を一切検証せず
   - `src/machina/routes.ts:780-798` (Discord) も X-Signature-Ed25519 を検証せず
   - 一方 `src/discord-hook/interactions.ts` で `verifyDiscordInteraction()` が Ed25519 検証を実装したが、 routes.ts で呼ばれていない
2. **JWT_SECRET の default 値が env-cli.config.ts:26 でハードコード続行**
3. **Discord Interactions endpoint (新規) の導入不完全**

### C. 実装の問題

1. `resolveAssignee` が TODO 放置のまま空結果 (`src/machina/webhook-handler.ts:247`)
2. Discatier Core phase2 synthesis-handlers.test.ts が failing 状態で main merge
3. session race condition の複雑化 (MACHINA + Discatier 二重 session state)
4. completion 二重発火の根本未対策 (`mode === "none"` でも completion check 走る)

### D. 不完全な機能

1. Discord pivot (PR #18) が未 merge
2. `checkGroupAccess` がスタブのまま (`src/machina/routes.ts:72-83` で `Promise.resolve([])`)
3. Discussion Sessions の永続化 (Di-9) が未実装

## 推奨優先順位

**CRITICAL:**
1. Slack `X-Slack-Signature` HMAC-SHA256 検証 + Discord `X-Signature-Ed25519` Ed25519 検証
2. `verifyDiscordInteraction()` を `/api/discord/interactions` で利用

**HIGH:**
3. `resolveAssignee` の実装着手
4. phase2 test failure 修正 + CI に merge gate 追加
5. `mode === "none"` の completion check を early return

**MEDIUM:**
6. JWT_SECRET production guard
7. MACHINA routes 分割
8. Discatier session 永続化 (Di-9)

詳細は各レビュー MD を参照。

**評価基準:** A=ベストプラクティス / B=軽微改善 / C=リリース前要対応 / D=即時対応必要
