# REVIEW_DESIGN — Discutere (2026-05-13)

評価: **B**

## 全体像

Discutere は Slack/Discord の chat-to-task 自動化サービス。Hono backend + React 19 frontend + SQLite/Drizzle、認証は Cernere Composite (HttpOnly Cookie + 自前 service_token)。`src/machina/PLAN.md` で M3 MACHINA モジュールとして詳細設計されており、`README.md` でも API と DB スキーマがほぼ最新の実装と一致する。設計書からの逸脱は限定的で、AIFormat 流の構成 (Hono + Drizzle + Cernere + Infisical env-cli + concurrently dev script) が忠実に踏襲されている。

## 強み

- **チャンネルモードによる責務分離が明確**: `src/shared/constants.ts:19` の `CHANNEL_MODES = ["task","discussion","none"]` をキーに、`src/machina/task-mode.ts` と `src/machina/discussion-mode.ts` が完全に独立。webhook 受信 (`src/machina/webhook-handler.ts:111`) は normalize → monitor 解決 → mode 分岐の 1 経路に統一されており、追加モード拡張がしやすい (`src/machina/webhook-handler.ts:163`)。
- **オンメモリセッションの取り扱いを明示**: `src/machina/mode-state.ts` で task / discussion 両セッションを Map で保持しつつ、シリアライザを別関数化 (`serializeTaskSession` 等) して timer ハンドルを外部に漏らさない設計。README §「Channel Mode Sessions」でも「プロセス再起動で消える」と明記しており、設計意図と実装が一致する (`README.md:169`)。
- **LLM フォールバック設計**: `src/machina/haiku-classifier.ts:36` で `ANTHROPIC_API_KEY` 未設定時は ruleset (`analyzer.ts`) にフォールバック。LLM 障害時にもサービスが停止しないようになっている。
- **アダプタパターンによる PM 連携**: `src/machina/pm-relay.ts` の `registerPmRelayAdapter` で M2 (PM) を後付けできる構造。`PLAN.md` §3.1 の Phase 設計と整合する。

## 弱み・課題

- **Cernere Composite vs LUDIARS 標準のズレ**: ユーザメモリ `feedback_cernere_auth_only_endpoints.md` で「Cernere は `/auth` しか開かない / `/oauth/*`,`/ws/service` は存在しない」とされ、`/api/auth/project-token` 経路に移行中だが、`src/auth/composite.ts:50` は依然 `/api/auth/exchange` を叩く。README にも「Cernere Composite」とのみ記載されており、project-token への移行プランが設計書側で未反映 (`README.md:108`).
- **`checkGroupAccess` がスタブ**: `src/machina/routes.ts:72` の `memberships = await Promise.resolve([])` は常に空配列を返す。admin 以外はすべての group/workspace アクセスが 403 になる設計だが、ローカル開発時の挙動・本番アクセス制御戦略がドキュメント化されていない。設計上は Cernere に workspace membership を問い合わせるべきだが、TODO すらコメントに無い。
- **PLAN.md と実コード/README の不整合**: `PLAN.md:34` は `modules/machina/` パスを前提に書かれているが実体は `src/machina/`。`PLAN.md:354` のテーブル名 `machina_tasks` / `machina_channel_monitors` も実装では `tasks` / `channel_monitors` (`src/db/schema.ts:131,28`)。Discutere 独立後の rename が反映されていない。
- **DB が SQLite 単一ファイルでマルチ workspace を抱える設計**: README §「AIFormat 準拠」で「PostgreSQL 移行は将来課題」と明記されているが、`workspace_id` を全テーブルに持たせる構造 (`src/db/schema.ts:34,77,135`) は workspace 分離のため意義はあるものの、SQLite 単一ファイルでは書込競合 (WAL でも) や論理 backup が辛い。設計書に水平分割方針が無い。
- **完了キーワード検出のスコープ**: `src/machina/webhook-handler.ts:197` で `findByWorkspaceIdAndStatus(workspaceId, "pending")` を呼び workspace 全タスクを毎メッセージ全取得している。設計書側に「対象は同一 channel に紐づく task のみ」など絞り込みポリシーが書かれていない。

## 推奨アクション

1. PLAN.md を Discutere 独立版に更新 (パス/テーブル名/`workspaceId`)。`/groups/:workspaceId/...` 表記との突合せ。
2. README に Cernere `/api/auth/project-token` 移行ロードマップを追記。`feedback_cernere_auth_only_endpoints.md` の現状と整合を取る。
3. `checkGroupAccess` の本来の要件 (Cernere に問い合わせる / project-token 内の workspace claim を見る) を設計書に明文化し、TODO コメントを残す。
4. 完了キーワード検出の対象範囲 (workspace vs monitor vs thread) を仕様化。
