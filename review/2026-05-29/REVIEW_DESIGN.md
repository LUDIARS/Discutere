# REVIEW_DESIGN — Discutere (2026-05-29)

評価: **B+**

## 全体構成

Discutere は 2 層アーキテクチャ:
1. **MACHINA layer** (`src/machina/`): Slack/Discord chat-to-task, 従来の webhook 処理
2. **Discatier Core layer** (`src/core/`): Event Sourcing + Projection (Phase 1~6), ゲーム設計仮説ライフサイクル

Discord-only 移行 (docs/codex-tasks/2026-05-28-discord-only-pivot.md) により、 frontend + Cernere REST は廃止予定だが、 過渡期設計のままになっている。

## 1. 設計強度

| 評価 | 観点 | 所見 |
|------|------|------|
| A | Event Sourcing immutability | `src/core/events/event-log.ts` で append-only log、 projection は idempotent。 phase 1~6 phase transitions が state machine 化 (`src/core/hypothesis/state-machine.ts:30-50`) され、 deadlock risk 低い |
| B | 障害分離 | MACHINA layer は webhook failure で console.error + 非ブロッキング (`src/machina/routes.ts:767-772`)。 Discatier Core は Event 書き込み failed → Event.status=error で記録。 ただし session recovery strategy が未定義 |
| B | 冪等性 | Core 側は event idempotent key (event_id) で重複排除可能。 MACHINA 側は webhook 再受信時に同じ message_id で appendToExisting の risk あり (`src/machina/webhook-handler.ts:129`) |
| C | 入力バリデーション | POST body が `c.req.json<{...}>()` キャスト直後、 zod schema がないため enum 以外は実行時検証なし |
| C | エラーハンドリング | fetch failure が throw / console.error で統一されず。 Result<T,E> 型がないため error context が失われやすい |
| B | リトライ・タイムアウト | 外部 API (Claude Haiku, GitHub Discussion, Cernere exchange) の timeout が個別実装。 共有 timeout 定数なし |

## 2. 設計思想の一貫性

| 該当箇所 | 逸脱内容 | 本来の設計思想 | 推奨修正 |
|----------|---------|--------------|---------|
| `src/auth/composite.ts:30` | `JWT_SECRET` default value が "dev-secret-..." ハードコード | env-cli で infraKeys は「dev デフォ + Infisical 上書き」だが、 JWT 等の秘密鍵は per-user memory-only か remote-only 方針と矛盾 | startup guard で production 起動時に default 拒否 |
| `src/machina/routes.ts:100-102` | `await import("../db/repository.js")` 動的 import で N+1 query | repository layer の DI は冒頭で一度だけ | userRepo を import に上げる |
| `src/machina/webhook-handler.ts:197-210` | `findByWorkspaceIdAndStatus` で workspace 全タスク取得 + for loop update | README §23「none は何もしない」との矛盾 | early return で mode === none を最初に check |
| `src/machina/routes.ts:50-55` | `logMachina(userId, action, ...)` が `console.log` にすり替わり | LUDIARS audit log 機能を予定していた | logger strategy を決定 (実装 or 削除) |
| `src/core/projection/message-input.ts` vs `src/machina/task-mode.ts` | Discatier と MACHINA が独立した session state を持つ | 単一 truth source (Event Log) を想定 | message → submitMessage → Event emitted のパスを整理 |

## 3. モジュール分割度 / 凝集度

| モジュール / ファイル | 凝集度評価 | 所見 |
|-------------------|-----------|------|
| `src/machina/routes.ts` (980 行) | 手続き的 | タスク CRUD, monitor CRUD, session control, webhook 受信が 1 file に混在 |
| `src/machina/webhook-handler.ts` (300+ 行) | 通信的 | slack/discord message normalize → task-mode/discussion-mode delegate で role 自体は明確だが、 completion check の責務が混在 |
| `src/core/bridge/translation/pipeline.ts` (36 行) | 機能的 | MockLlmClient / AnthropicLlmClient switch point として責務が明確 |
| `src/core/events/event-log.ts` (89 行) | 機能的 | append-only, idempotent key, projection に特化 |
| `src/core/projection/` (50+ files) | 機能的 | command-parser → learning/emotion/synthesis 分岐 → cross-query の responsibility が file 分割で明確 |
| `src/machina/task-mode.ts` (300 行) | 手続き的・時間的 | Haiku classify wait → hearing orchestration → resume/dismiss session のタイムシーケンスが絡み合う |

## 循環依存 / 結合度

- ❌ `src/machina/task-mode.ts` → `src/db/repository.ts` → `src/machina/routes.ts` (構造的循環性はないが coupling が高い)
- ✓ `src/core/events/event-log.ts` → `src/core/events/projection.ts` → repositories (単方向)
- ⚠️ `src/machina/webhook-handler.ts` → `submitMessage(core/projection)` → `src/core/index.ts:createCore()` で MACHINA が Core に依存するが、 reverse path がない良好な依存方向

## 総合評価

| # | 観点 | 評価 | 重大指摘数 |
|---|------|------|-----------|
| 1 | 設計強度 | B | 2 (入力 validation, error handling) |
| 2 | 設計思想の一貫性 | B+ | 3 (default JWT, logMachina dead, session state duplicate) |
| 3 | モジュール分割度 | B | 2 (routes.ts 大型化, task-mode state machine 複雑化) |

**前回 B から B+ への改善点**: Discatier Core の Event Sourcing と Projection layer が architectural clarity を大幅に向上。
**懸念**: MACHINA layer の過渡期設計が Discord-only pivot の前に整理されるべき。
