# REVIEW_IMPLEMENTATION — Discutere (2026-05-29)

評価: **C**

## ロジック上の問題

| 優先度 | 箇所 | 問題 | 影響 |
|--------|------|------|------|
| CRITICAL | `src/machina/routes.ts:750-798` | webhook 署名検証欠落 + routes で verifyDiscordInteraction 呼び忘れ | 第三者メッセージ投入、 Haiku 課金漏洩 |
| HIGH | `src/machina/webhook-handler.ts:155-165` | mode=none でも completion check 実行 | 無関係な完了 talk で task 全件 done 化 |
| HIGH | `src/machina/webhook-handler.ts:247-263` | resolveAssignee が userIds=[] のまま TODO 放置 | completion 検出後 assignee が resolve されず task 更新 no-op |
| HIGH | `src/machina/task-mode.ts:238-250` | resumeSession が taskSessionStore 同時書き換え (lock なし) | webhook と resumeSession が並行すると messages[] 配列が消える race |
| HIGH | `src/machina/discussion-mode.ts:38-48` | scheduleDiscussionDigest で _timer 書き戻し後 shallow copy で他フィールド上書き | status: "summarizing" が古い値で覆われ session 状態不正 |
| MEDIUM | `src/machina/webhook-handler.ts:198-210` | handleTaskCompletion が findByWorkspaceIdAndStatus を 2 回 call | 毎メッセージで workspace 全 task 取得 O(messages × tasks) degradation |
| MEDIUM | `src/machina/routes.ts:97-116` | enrichedTasks 用 userRepo fetch が N+1 | タスク 100 件 = DB call 101 回 |
| MEDIUM | `src/machina/routes.ts:100,131` | dynamic import("../db/repository.js") の濫用 | repository は既に冒頭 import 済みなのに毎回 import |
| MEDIUM | `src/machina/analyzer.ts:75` | extractDueDate regex が「5日後」「13日」を扱えない | 期日解釈の漏れ |
| MEDIUM | `src/machina/webhook-handler.ts:269-273` | extractSlackMentions が `<!subteam^X>` 未対応 | team mention 無視 |
| MEDIUM | `tests/core/projection/synthesis-handlers.test.ts` | npm run test:phase2 で failing (pre-existing, docs に明示) | main merge 後だが CI 未構成のため無視 |

## 型/構造の不備

| 箇所 | 問題 | 推奨修正 |
|------|------|---------|
| `src/middleware/auth.ts:51-55` | `c.set("userId" as never, ...)` で型無視 | Hono `Variables` declare module で型安全確保 |
| `src/machina/routes.ts:152,228,454` | `c.req.json<{...}>()` 直後 schema 検証なし | zod schema 定義 + `@hono/zod-validator` |
| `src/machina/webhook-handler.ts:63` | 戻り型 `Promise<Record<string, unknown> | null>` 緩い | union type 化 |
| `src/discord-hook/interactions.ts:24` | `interactionToCommand(payload: any)` 型 any | InteractionPayload interface 定義 |

## エラーハンドリング

- **`fetch` failure 処理不一致**: composite.ts:50 throw, chat-reply.ts:90 throw, github-discussion.ts:74 throw, webhook-handler は console.error 握り潰し
- **GitHub publish failure silent**: `src/machina/discussion-mode.ts:159` で success 時のみ session 削除。 failure 時 status="failed" mark されず session orphan
- **Timeout 戦略未定義**: shared timeout constant 不在

## ファイル別レビュー

**`src/machina/routes.ts` (980 lines)**
- single file に task/monitor/webhook/analyze CRUD 混在
- `checkGroupAccess` stub (権限検証なし)
- dynamic import 濫用
- ハンドラ開始の 6 行重複 (userId/role/workspaceId 抽出)
- 推奨: routes 分割 → task/monitor/webhook/analyze

**`src/machina/webhook-handler.ts`**
- mode check 後に completion check (意図不明)
- resolveAssignee TODO 放置
- extractDueDate regex 不足
- normalize + delegate 構造は良い

**`src/core/events/event-log.ts`**
- append-only + idempotent key 完成
- projection consistency check 充実

**`tests/core/`**
- phase 1~6 test coverage 充分 (24 files)
- phase2 synthesis-handlers.test.ts failing
- MACHINA layer unit test 皆無
