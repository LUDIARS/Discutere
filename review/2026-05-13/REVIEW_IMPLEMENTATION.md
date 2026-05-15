# REVIEW_IMPLEMENTATION — Discutere (2026-05-13)

評価: **C**

## バグ / ロジック上の問題

- **`processMessage` の completion 二重発火**: `src/machina/webhook-handler.ts:150` で `analyzeMessage` を毎メッセージ呼んで `isCompletion` を判定し、`handleTaskCompletion` を呼ぶ。`mode === "none"` でもこのパスは通るため、README §「`none`: 何もしない (ログ保存のみ)」(`README.md:23`) と矛盾する。none チャネルでも完了キーワードがあれば task 全件 done 化が走る。
- **`appendToExisting` の status 取り回し**: `src/machina/task-mode.ts:122` の hearing 分岐で再分類して登録が成功すると `taskSessionStore.update(session.id, { status:"registering", taskId })` してから `remove` するが、その間も同スレッドの新着が `findActiveByThread` で拾われる可能性がある。`mode-state.ts:100` のコメントに「最終状態のものは除外」とあるが、実コードは除外していない (`return s` でそのまま返す)。collecting/registering 中の race を放置している。
- **`resumeSession` が race を生む**: `src/machina/task-mode.ts:238` は webhook 経由の `handleTaskModeMessage` と同じ session を同時に書き換える。`taskSessionStore.update` は単純な置換でロックがないため、補足注入と webhook 追記が交差すると messages 配列が消える。
- **`scheduleDiscussionDigest` の timer 再登録**: `src/machina/discussion-mode.ts:38` で既存セッションのタイマーをクリアしてから `setTimeout` を発行し、その後 `update(session.id, { _timer: timer })` で書き戻すが、`update` は浅いコピーで `_timer` を上書きするため、既存の他フィールド (`status: "summarizing"` 等) が同時に書き換わると古い値で覆い被さるリスクがある。
- **`handleTaskCompletion` の `findByWorkspaceIdAndStatus` を 2 回**: `src/machina/webhook-handler.ts:198-204`。これは workspace の `pending` / `in_progress` 全件を毎メッセージ取りに行く。タスクが増えると O(messages × tasks) で線形に劣化する。
- **`enrichedTasks` の N+1**: `src/machina/routes.ts:97-102`。`Promise.all` も使わず `await import("../db/repository.js").then(...)` を for ループで個別実行している。同じ理由で `routes.ts:130` のタスク詳細でも 1 件だけだが dynamic import を毎回行うのが無駄。
- **`dynamic import` の濫用**: `src/machina/routes.ts:100`,`131` で `await import("../db/repository.js")` が呼ばれているが、既にファイル冒頭で `import { taskRepo, ... } from "../db/repository.js"` 済み (`routes.ts:28`)。`userRepo` を最初の import に足せば十分。
- **`logActivity` が console.log にすり替わったまま放置**: `src/machina/routes.ts:50-55` `// logActivity removed` のコメントと共に `console.log` の wrapper になっている。実質ログ収集機能なし。
- **`getUserId` の anonymous チェック不一致**: `src/middleware/auth.ts:34` で `"anonymous"` をセットするが、`routes.ts` の各エンドポイント (`routes.ts:83` 等) は `if (!userId)` だけで弾く。anonymous は truthy なのでスルーされ、結果的に `checkGroupAccess(workspaceId, "anonymous", "general")` が走り 403 になる動線でしか守られていない。`anonymous` を 401 にするミドルウェアが無い。
- **`extractDueDate` の `M/D` パターンが `\d+日後` と競合**: `src/machina/analyzer.ts:75` の `(\d{1,2})[/\-月](\d{1,2})(?:日)?` は `5月13日` を捕まえるが、`5日後` のテキストでも前方マッチに失敗するケースがあり、日本語の `13日` 単独入力では (月情報がないので) マッチしない。仕様外文字列の挙動が曖昧。
- **`extractSlackMentions` がチームメンション `<!subteam^X>` を未対応** (`src/machina/webhook-handler.ts:264`)。

## 型/構造の不備

- **`c.set("userId" as never, ...)`**: `src/middleware/auth.ts:51-55` は型を `never` でキャストして潰している。Hono の `Variables` を declare すれば本来不要 (`c.set("userId", id)` で型安全)。LUDIARS 標準パターンと比較しても古い書き方。
- **`c.req.json<{ ... }>()` 直後にバリデーションなし**: `src/machina/routes.ts:152`,`228`,`454` 等。zod 等の入力 schema が無いため、`platform`,`mode`,`priority` のような enum 以外は実行時に弾けない。型 cast が現実を保証しない。
- **`Promise<Record<string, unknown> | null>`**: `src/machina/webhook-handler.ts:63` の戻り値型が緩い。`{ mode: "task", action: ... } | { mode: "discussion" } | null` の判別可能 union にするべき。

## エラーハンドリング

- **`fetch` failure 時の挙動が場所毎にバラバラ**: `composite.ts:50` は throw、`chat-reply.ts:90` は throw、`github-discussion.ts:74` も throw。一方 webhook-handler は `console.error` で握り潰し。集約された Error class や `Result` 型がない。
- **`runDiscussionDigest`**: `src/machina/discussion-mode.ts:159` で GitHub publish 成功時は `status: "summarizing"` のまま `remove(sessionId)` する流れだが、`status: "completed"` のような完了状態を持たないため `failed` か削除のみの 2 値しかフロントに見えない。
