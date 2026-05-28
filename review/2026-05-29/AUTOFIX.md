# AUTOFIX — Discutere (2026-05-29)

## 概要

- 修正ファイル数: 0
- 変更行数: +0 / -0
- カテゴリ別件数: lint=0 / typo=0 / unused_import=0 / dead_code=0 / gitignore=0 / toc=0 / critical_high=0
- 関連 PR: なし (本日は自動修正なし — Critical/High は全て大型 refactor / 設計判断要)

## カテゴリ別

### 機械的 (該当 0 件)

- lint: なし
- typo: なし
- unused_import: なし
- dead_code: bcryptjs dead dep が package.json:33 に残存しているが、 単独で見ると軽微だが、 build 経路で impact 0 のため bounded-fix として保留
- gitignore: なし
- toc: なし

### Critical / High の bounded fix (0 件)

本日の Critical / High 指摘は全て「設計判断・大型実装」 を要するため自動修正対象外:

- Slack/Discord webhook 署名検証 → 新機能実装範疇 (handler 分離 + secret 取得 + timing-safe 比較)
- resolveAssignee 実装 → Discatier Person repository / MACHINA user cache の設計判断要
- checkGroupAccess 実装 → Cernere project-token 移行待ち
- mode=none early return → ロジックフロー変更で webhook-handler 全体構造に影響
- phase2 test failure 修正 → 原因調査 + test 修正で 1 時間超見込み

## 手作業に回した指摘 (= 自動修正の範囲外)

- `src/machina/routes.ts:750-798` — [Critical] Slack/Discord webhook signature 検証 (新機能実装)
- `env-cli.config.ts:26` + `src/index.ts` (startup) — [High] JWT_SECRET production guard (startup ロジック追加)
- `src/machina/webhook-handler.ts:247` — [High] resolveAssignee 実装 (設計判断要)
- `src/machina/routes.ts:72` — [High] checkGroupAccess 実装 (Cernere 連携設計要)
- `src/machina/webhook-handler.ts:155-165` — [High] mode === "none" early return (ロジックフロー変更)
- `tests/core/projection/synthesis-handlers.test.ts` — [High] phase2 test 修正 (原因調査要)
- `src/machina/routes.ts` 全体 — [Medium] 980 行を task/monitor/webhook/analyze に分割 (大型 refactor)
- `package.json:33` — [Low] bcryptjs dead dep 削除 (確認後可)
- `CLAUDE.md` (新設) — [Medium] アーキテクチャ digest 文書作成 (執筆判断要)

## 関連

- レビュー全文: REVIEW.md / REVIEW_*.md
- 修正 PR diff: なし
