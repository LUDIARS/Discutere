# AUTOFIX — Discutere (2026-05-13)

`autofix_count: 0` — 本レビューはコード修正禁止のため、列挙のみ。後続 PR の候補として残す。

## 安全に自動修正できる候補 (列挙のみ)

### lint / typo / 軽微クリーンアップ
- `src/machina/routes.ts:50-55` の dead wrapper `logMachina` を削除し、内部の `console.log` を直書きに統合 (または pino 導入)。
- `src/machina/routes.ts:100,131` の `await import("../db/repository.js").then(m => m.userRepo...)` を冒頭 import (`taskRepo, taskLogRepo, ...`) に `userRepo` を追加するだけで除去。
- `package.json:27` の `bcryptjs` / `@types/bcryptjs` (devDeps) を `grep` で参照 0 と確認後、`npm uninstall`。
- `src/machina/github-discussion.ts:135` の `/general|general|雑談|ディスカッション/i` 重複 `general` を 1 つに。
- `src/index.ts:13` の `frontendUrl` フォールバック `"http://localhost:5174"` を `env-cli.config.ts` の defaults と一致させる定数化。
- `src/machina/discussion-mode.ts:160` `status: "summarizing"` のまま `remove` する箇所を `"completed"` (新規 enum) または status 更新を削除して整合。

### 簡易セキュリティ
- `env-cli.config.ts:26` の `JWT_SECRET` default 値をコード側でガード: `src/index.ts` 起動時に `process.env.JWT_SECRET === defaultValue && NODE_ENV === "production"` で fatal exit。
- `src/middleware/auth.ts:62` の `X-User-Id` ヘッダーフォールバックに `process.env.ALLOW_HEADER_AUTH === "1"` の二段スイッチ。
- `src/index.ts:19` の `allowHeaders` から `X-User-Id`,`X-User-Role` を production では外す。
- `src/machina/routes.ts:798` `/analyze` の `body.text.length > 8192` で 413 を返す。
- `src/auth/routes.ts:53` の Cernere エラーメッセージを `"authentication failed"` の汎用文に。

### 型 / DX
- `declare module "hono" { interface ContextVariableMap { userId: string; userRole: string; userName: string; userEmail: string | null; } }` を `src/types/hono.d.ts` に追加し、`src/middleware/auth.ts` 内の `c.set(...as never...)` を全削除。
- `c.req.json<T>()` 周辺を `@hono/zod-validator` で書き直し (PR が大きくなるので慎重に)。

### テスト整備
- `vitest` を devDep に追加し、`scripts.test` を追加。`src/machina/analyzer.test.ts` で due-date resolver / priority 判定の最低限のテストを作る。

## 非自動修正 (要設計判断)
- Webhook 署名検証実装 (`/webhook/slack`, `/webhook/discord`)。
- `checkGroupAccess` の本実装 (Cernere project-token claim 経路)。
- bot token の at-rest 暗号化 or in-memory 化 (feedback_secret_per_user_memory_only.md 整合)。
- mode-state の race fix (ロックまたは serialize)。
- completion 検出スコープの仕様確定。
