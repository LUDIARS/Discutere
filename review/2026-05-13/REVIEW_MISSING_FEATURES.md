# REVIEW_MISSING_FEATURES — Discutere (2026-05-13)

評価: **C**

## README に書かれているが未実装/不完全

- **Workspace アクセス制御**: README `## API Endpoints` 全エンドポイントが `/api/groups/:workspaceId/...` で workspace スコープを切るが、`src/machina/routes.ts:72` の `checkGroupAccess` は `admin` 以外を全て弾くスタブ。Cernere の `/api/auth/project-token` (per-user × per-project) や workspace membership の問い合わせ機構が未実装。実質 admin 専用サービス。
- **完了検知のスコープ絞り込み**: README §「完了の検出」(`README.md:208`) は単純なキーワード列挙だが、`src/machina/webhook-handler.ts:197` は対象 workspace 全 task を done 化する。「同一 channel の task のみ」「assignee 一致のみ」のような最低限のスコーピングが未実装。
- **通知イベント**: `PLAN.md:84` で `EVENT_NAMES.MACHINA_TASK_CREATED` 等を定義しているが実装なし (`src/machina/webhook-handler.ts:229` `// TODO: notification events via Cernere relay`)。
- **`resolveAssignee` 未実装**: `src/machina/webhook-handler.ts:241` `// TODO: resolve workspace members via Cernere`。`userIds: string[] = []` 固定なのでアサイン解決は完了キーワードでのタスク close で必ず空振りする。
- **Critical Path 判定**: PLAN.md §2.3「クリティカルパス判定 (将来拡張)」は完全に未実装。`tasks.isCriticalPath` フラグだけ存在し、設定経路は PUT で手動更新のみ。

## CLAUDE.md / spec/ の欠落

- `E:/Document/Ars/Discutere/CLAUDE.md` が存在しない。LUDIARS 標準では各リポに CLAUDE.md (Claude Code 向けの実装規約・モジュール表) を置く慣行があるが Discutere には未整備。
- `spec/` ディレクトリ無し。`src/machina/PLAN.md` 1 本のみで AIFormat の DESIGN.md / API_SPEC.md 相当が分散。`PLAN.md` 自体も Discutere 独立前の `modules/machina/` パス前提 (`PLAN.md:11`) のままで陳腐化している。

## テスト / 開発体験

- **テストコード皆無**: `src/` 配下に `*.test.ts` / `__tests__/` / `vitest.config.ts` 無し。`package.json:7` の `scripts` に `test` エントリすら無い。Webhook 入力 normalize、analyzer の優先度判定、due date resolver など、ロジックの濃い部分にテストが必要。
- **CI/Linter 設定がリポに無い**: `eslint.config.*`, `.github/workflows/*` を git ls-files で確認できず。Hono アプリの最低限の typecheck (`tsc --noEmit`) 自動化が無い。
- **Logger / observability**: `console.log` / `console.error` の生 print のみ。pino 等の構造化ログ、request id、Cernere relay への audit log が未実装。`logMachina` (`src/machina/routes.ts:53`) も console.log の wrapper。Excubitor (`project_excubitor.md`) との接続点もない。

## 機能上の欠け

- **Webhook 署名検証**: schema には `botSigningSecret` カラムがあるのに (`src/db/schema.ts:43`)、`/webhook/slack`,`/webhook/discord` で利用していない (REVIEW_VULNERABILITY も参照)。
- **Slack 以外のイベント種**: `app_mention`,`reaction_added`,`channel_join` などが未対応。`message.subtype` を見る `event.subtype` ガードのみ。
- **Discord interaction (slash command)** 受信なし。`!task`/`/task` はメッセージ本文の prefix だけで Discord native の slash command は未対応。
- **Pagination / 検索**: `GET /api/groups/:workspaceId/tasks` (`src/machina/routes.ts:81`) は status フィルタのみで、limit / offset / 検索 query 未実装。
- **タスクログのフィルタ・ページング**: `GET .../tasks/:taskId/logs` 同様。
- **summaries のページング**: `chatSummaryRepo.findByMonitorId` (`src/db/repository.ts` 末尾、想定) は全件返す前提に見える。
- **Bot disconnect**: 監視チャネル DELETE で Slack/Discord 側の bot 退出 API は呼ばない。
- **`/me` 以外の auth 情報取得**: refresh エンドポイントなし。`TOKEN_COOKIE_MAX_AGE = 900` (15 min) が切れたらユーザは再 popup ログインを強いられる (`src/auth/routes.ts:19`)。Cernere refresh token (`composite.ts:62` で受け取っているが捨てている) を活用していない。
- **`logout` の Cernere 連携**: Cernere 側のセッション無効化を呼ばないため、popup で別タブからの再ログインが「即座に成立」する。シングルサインアウト未対応。
- **frontend pages の不足**: README §「タブ構成」(PLAN.md 379) で「タスク/監視設定/テキスト解析」の 3 タブを謳うが `frontend/src/pages/` は `LoginPage / CallbackPage / MachinaPage` の 3 ファイルのみで、`MachinaPage` 単体に詰め込まれているはず (内部構造は未確認)。
