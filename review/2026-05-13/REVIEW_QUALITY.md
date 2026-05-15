# REVIEW_QUALITY — Discutere (2026-05-13)

評価: **B**

## 良いところ

- **ファイル分割と命名が機能ごとに揃っている**: `src/machina/{analyzer,haiku-classifier,task-mode,discussion-mode,mode-state,chat-reply,github-discussion,webhook-handler,routes,pm-relay,summarizer}.ts` で 1 ファイル 1 責務を保っている。
- **JSDoc / コメントが日本語で密**: `src/machina/task-mode.ts:1-13`,`mode-state.ts:1-10`,`discussion-mode.ts:1-13` などに、フロー図と意図がまとまっており、初見でも処理経路を追える。
- **`const TASK_STATUSES` / `CHANNEL_MODES` を `as const` + `(typeof ...)[number]`** で型生成 (`src/shared/constants.ts`)。LUDIARS 標準パターンに沿う。
- **Drizzle schema が `index`/`unique` を明示**: `src/db/schema.ts:62-65`,`92-97` で複合 unique と検索インデックスを冒頭から張っている。

## 改善余地

- **型キャストの濫用**: `src/middleware/auth.ts:51-55` `c.set("userId" as never, userId as never)`、`routes.ts:762` `event as unknown as Parameters<typeof handleSlackMessage>[0]`、`routes.ts:785` 同様。Hono の `ContextVariableMap` (typed Variables) を `declare module "hono"` で宣言すれば `as never` も `unknown` も消える。型の力を捨てる癖が散見される。
- **`c.req.json<T>()` の出力を信用しすぎ**: `routes.ts:152`,`228`,`454`,`525` 等で受け取った body を直接 DB に書き込んでいる。`@hono/zod-validator` の導入が望ましい。
- **dynamic import**: `routes.ts:100`,`131` の `await import("../db/repository.js")` は遅延ロードの意図が無く単に書き方の問題。先頭 import に統合可。
- **重複コード**: `routes.ts` の各 handler 冒頭 6 行 (`getUserId / 401 / systemRole / workspaceId / checkGroupAccess`) がほぼ全てで複製されている。`machinaRoutes.use(...)` ミドルウェアで一括化できる (28 箇所)。
- **マジックナンバ**: `routes.ts:605` `Math.min(500, Math.max(1, ...))` の上限 500、`routes.ts:863` `5 * 60_000` の停滞閾値、`task-mode.ts:82` `confidence < 0.4`、`webhook-handler.ts:163` `delayMinutes ?? 5` 等。`shared/constants.ts` に集約推奨。
- **`logMachina`**: `src/machina/routes.ts:50-55` でコメントが「logActivity removed」と残骸状態。死んだ wrapper を削除し、`console.log` を直書きするか pino を導入するか統一すべき。
- **`as` キャスト後の noop 関数**: `src/machina/webhook-handler.ts:62` の戻り型 `Promise<Record<string, unknown> | null>` は曖昧。判別可能 union に置き換えると caller のロジックも tighten できる。
- **コメントと実装の乖離**: `mode-state.ts:104` 「最終状態のものは除外」と書いてあるが除外していない (REVIEW_IMPLEMENTATION 参照)。
- **`src/index.ts:39-43` の console.log** はサービス起動メッセージとして console に直書き。本番では構造化ログ推奨。

## ファイル単位

| ファイル | LOC 規模 | 寸評 |
|---|---|---|
| `src/machina/routes.ts` | ~980 行 | 単一ファイルが太い。tasks / monitors / sessions / webhooks / summaries で 5 分割推奨 |
| `src/machina/webhook-handler.ts` | ~270 行 | normalize + dispatch + completion を兼ねており肥大。completion を別モジュールへ |
| `src/machina/task-mode.ts` | ~290 行 | 単一責任に近いが `appendToExisting` / `registerTask` / `resumeSession` で重複ロジック |
| `src/middleware/auth.ts` | ~110 行 | typed Variables 移行で半分にできる |

## 依存・パッケージ

- `package.json:25-46` は最低限のみで OK。
- ただし `bcryptjs` は `dependencies` に入っているが grep で利用箇所が見当たらない (composite.ts/middleware は `jsonwebtoken` のみ)。dead dependency の可能性。
- `dist/` が `.gitignore` 対象か未確認 (`ls` で見えた)。git に commit されているなら除去推奨。
