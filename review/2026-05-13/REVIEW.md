# REVIEW — Discutere (2026-05-13)

総合評価: **C+ (weighted_score 65)**

| 観点 | 評価 | サマリ |
|---|---|---|
| DESIGN | B | チャンネルモード分離・Cernere Composite 採用・アダプタパターンは設計として整っているが、PLAN.md がリポ独立前 (`modules/machina/`) のまま陳腐化、`checkGroupAccess` が設計書に出てこない |
| VULNERABILITY | C | `JWT_SECRET` ハードコード default、Slack/Discord webhook 署名検証ゼロ、`/analyze` 長制限なし、dev ヘッダー認証フォールバックなど、High が複数 |
| IMPLEMENTATION | C | completion 検出が `mode=none` でも走る、session race、N+1 / dynamic import、`as never` キャスト多用、入力 schema validation 不在 |
| MISSING | C | `checkGroupAccess` スタブ、`resolveAssignee` TODO 放置、通知イベント未実装、テスト/CI 皆無、CLAUDE.md 不在、refresh token 経路無し |
| QUALITY | B | コメントとファイル分割は良いが、routes.ts 980 行が単一、各 handler 冒頭 6 行重複、マジックナンバ散在、dead wrapper (`logMachina`) |

## 主な所見

1. **Webhook 署名検証の欠落 (HIGH)** — `src/db/schema.ts:43` に `bot_signing_secret` がありながら、`src/machina/routes.ts:743,774` が完全 trust。Slack/Discord URL を知っている第三者が任意の `workspaceId` で偽メッセージを投入し、bot を踏み台に投稿/Haiku 課金消費が可能。
2. **`JWT_SECRET` のデフォルト値が `env-cli.config.ts:26` でハードコード** — `env:initialize` で Infisical にそのまま書き込まれる。`feedback_secret_per_user_memory_only.md` の方針 (per-user / memory only) とも整合しない。
3. **`checkGroupAccess` がスタブ** — `src/machina/routes.ts:72` で常に空配列を返すため、`admin` 以外の利用者は全 group/workspace に対し 403。Cernere の workspace membership 取得経路、または `/api/auth/project-token` (per-project) 移行が未完。
4. **completion 検出のスコープ過大** — `src/machina/webhook-handler.ts:197` が workspace 全 task を毎メッセージ取得して done 化する。`mode=none` でも実行されるため `README.md:23` の「none は何もしない」と矛盾。
5. **入力 schema validation 不在** — `routes.ts:152` 以降の `c.req.json<{...}>()` キャスト後にバリデーションが無く、`platform`/`mode`/`priority` を含む全 POST/PUT body が実質ノーチェック。
6. **テスト / CI / CLAUDE.md / spec 全て未整備** — analyzer の正規表現・due-date resolver や mode-state の race 箇所など、ユニットテストの価値が高い箇所が裸。
7. **デッドコード / 残骸** — `src/machina/routes.ts:50-55` `logMachina`、`package.json:27` の `bcryptjs` 依存、`dist/` のリポ内残置 (`git ls-files` 出力には含まれないが `ls` で存在)。

## 推奨優先順位

1. Slack `X-Slack-Signature` / Discord `X-Signature-Ed25519` の HMAC/Ed25519 検証実装。
2. `JWT_SECRET` の default 値ガード (`NODE_ENV=production` + default 値で起動拒否)。
3. `mode=none` の早期 return を webhook-handler の completion 経路の上に置く。
4. `checkGroupAccess` の実装方針を設計書化 + 実装着手 (Cernere project-token claim 参照)。
5. `@hono/zod-validator` の導入で全 POST/PUT body を schema 化。
6. `vitest` 導入 + analyzer / due-date / mode-state の最低限のテスト。

詳細は同フォルダの `REVIEW_DESIGN.md` / `REVIEW_VULNERABILITY.md` / `REVIEW_IMPLEMENTATION.md` / `REVIEW_MISSING_FEATURES.md` / `REVIEW_QUALITY.md` を参照。
