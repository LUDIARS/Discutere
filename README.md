# Discutere

**遊びの議論プラットフォーム (Discatier)** — ゲームデザインの「意図された体験 (`intended_affect`)」と
「観測された体験 (`expressed_affect`)」を 3 軸の弁証法的対話で突き合わせ、設計のズレ (`DesignGap`) と
跳躍的仮説 (`Hypothesis`) を育てる議論基盤。

> **役割整理 (2026-05-29)**
> 本リポジトリは当初 *Chat-to-Task 自動化 (Slack / Discord メッセージ → タスク生成)* として始まったが、
> その実装意図は **Imperativus (Iv) — 入力 → コマンド / タスクのルーター** が担うべきものと整理した。
> よって **Discutere は「議論専用 (Discatier)」に純化**し、Chat-to-Task 機能群は **Iv へ移管**する。
> 移管完了まではコードに Chat-to-Task 実装が同居するため、下記 Features では該当群に「→ Iv へ移管」を明記する。

## コンセプト — Discatier の 3 軸対話

| 軸 | モード | 役割 | 生成物 |
|---|---|---|---|
| Axis 1 学習対話 | `learning` | 語彙 / メカニクスの定義を精緻化 | `Mechanic.intended_affect` |
| Axis 2 感情会話 | `emotion` | プレイヤーが理論用語を介さず生の感情を報告 | `expressed_affect`(観測) |
| Axis 3 統合 | `synthesis` | Axis 1↔2 のズレに仮説を提示・検証 | `DesignGap` → `Hypothesis` |

単一のデータ基盤 (Kuzu グラフ + イベントログ) 上で Translation Bridge / Gap Detection /
Hypothesis Lifecycle が弁証法的ループを形成する。詳細は [`docs/discatier_implementation_plan.md`](docs/discatier_implementation_plan.md)。

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Backend | [Hono](https://hono.dev/) + Node.js 22+ (TypeScript) |
| Frontend | React 19 + React Router 7 + Vite |
| Database | SQLite (WAL) + [Drizzle ORM](https://orm.drizzle.team/) / Discatier Core は Kuzu (SQLite WAL) + イベントログ |
| Auth | [Cernere](https://github.com/LUDIARS/Cernere) Composite (HttpOnly Cookie + 独自 JWT) |
| Env/Secrets | [Infisical](https://infisical.com) + `@cernere/env-cli` |

## Features — 議論専用 (Discatier)

- **Discatier Core (3 軸対話)** — `src/core/` に Game / Mechanic / Aesthetic / Affect / PlayContext / Utterance / Reaction / DesignGap / Hypothesis を Event Sourcing + projection で管理。Translation Bridge / Gap Detection / Hypothesis Lifecycle。
- **Game KG クローラー (Phase 0)** — `src/crawler/` で著名ゲームの攻略データを `data/games/<slug>.md` から Discatier Core (`Game` / `Mechanic` / `Aesthetic`) に import。詳細は [`spec/crawler/DESIGN.md`](spec/crawler/DESIGN.md)
- **レビュー/コメント → 感情・議論ベクトル (Phase 0)** — 外部収集ツール (`LUDIARS/game-knowledge-graph/collectors/`) の収集物を、複合固定ベクトル (感情 + 評価アスペクト) + affect + 議論クラスタに変換。詳細は [`spec/crawler/SENTIMENT.md`](spec/crawler/SENTIMENT.md) / [`docs/crawler-to-discussion-pipeline.md`](docs/crawler-to-discussion-pipeline.md)
- **議論ソース可視化 (Phase 0)** — `src/visualize/` で hypothesis / gap / mechanic / aesthetic / utterance / session を 1 ノード = 1 md に書き出し、ノード間参照を `[[<type>:<id>]]` マジックリンクで繋ぐ。詳細は [`spec/visualize/DESIGN.md`](spec/visualize/DESIGN.md)
- **persona-engine (Phase 0)** — `src/persona-engine/` に閉じた議論駆動エンジン。ペルソナ (推進派 / 懐疑者 / 統合者 等 10 名) × チャットルール (propose-on-gap / refute-cold 等) × LLM 呼び出し。将来 `@ludiars/persona-engine` として切り出す前提。詳細は [`spec/persona-engine/DESIGN.md`](spec/persona-engine/DESIGN.md)
- **Discord で議論を動かす** — slash command (`/propose` `/validate` `/integrate` `/reject` `/discutere-status` 等) + persona-engine の自走。

### → Imperativus (Iv) へ移管予定 (旧 Chat-to-Task)

以下は本来 **Iv (入力→コマンド/タスクのルーター)** が担う実装。移管までは本リポに残存:

- **チャットからタスク自動生成** — Slack / Discord Webhook → パターンマッチでタスク検出・作成
- **チャンネルモード (task / discussion / none)** / **処理状況の可視化** / **タスク管理 (CRUD)**
- **BOT チャネル設定** / **チャットログ** / **チャット要約** / **テキスト解析プレビュー** / **外部 PM 連携** / **監査ログ**

## Game KG クローラー

```sh
# 既存 md を Discatier Core に取り込む
npm run crawl import data/games/sample-hollow-knight.md

# 登録済 Game / Mechanic / Aesthetic 一覧
npm run crawl list

# crawler モジュールのテスト (md round-trip + importer + GraphDB 読み)
npm run test:crawler
```

Phase 0 は md 手書き運用。Phase 1 で `runner.ts` に Claude API (WebSearch tool) を入れて自動 crawl + 日次 PR 化予定。
ToS / 引用ポリシーは `spec/crawler/DESIGN.md`「制約」セクション必読。

## 議論ソース可視化 (magic-link md)

```sh
# 仮説を md に書き出す → data/discussions/<workspace>/hypothesis/<id>.md
npm run visualize hypothesis <id>

# gap / mechanic / aesthetic / utterance / session も同様
npm run visualize gap <id>
npm run visualize mechanic <id>

# workspace 指定 (default: env DISCATIER_WORKSPACE or "knowledge")
npm run visualize hypothesis <id> --workspace team-alpha

# 全テスト (wikilink / md-exporter / dump)
npm run test:visualize
```

ノード間参照は `[[hyp:abc123]]` / `[[utt:550e8400]]` / `[[mch:ghi789]]` 形式で、 GitHub / Memoria / 専用 viewer で相互リンク可能。 viewer が wikilink → md ファイルパス (`data/discussions/<workspace>/<type-dir>/<id>.md`) を解決すれば graph として閲覧できる。

## Discord で議論を動かす (PR-I)

### 1. Discord application / bot 作成

1. <https://discord.com/developers/applications> で新規 application
2. Bot 追加、 Privileged Gateway Intents の "Message Content" を有効化 (必要なら)
3. OAuth2 URL Generator で `bot` + `applications.commands` scope + 必要権限を選んで guild に invite
4. Application の `INTERACTIONS ENDPOINT URL` を `https://<your-host>/api/discord/interactions?workspaceId=<ws>` に設定

### 2. monitor 登録 (Discutere 側)

`POST /api/groups/:workspaceId/monitors` で:

- `platform: "discord"`
- `channelId / channelName`
- `botToken`: Bot Token (= AI 発話 post に使用)
- `botSigningSecret`: Application Public Key (= Ed25519 検証 / Discord は public key を secret 欄に保存)
- `botWorkspaceId`: Guild ID

### 3. Slash command 登録

```sh
# DISCORD_BOT_TOKEN + DISCORD_APP_ID を export してから
curl -X POST -H "Authorization: Bot $DISCORD_BOT_TOKEN" -H "Content-Type: application/json" \
  -d '[
    {"name":"discutere-kill","description":"persona-engine の ON/OFF","options":[
      {"name":"enabled","description":"true で再開、 false で停止","type":5,"required":true}]},
    {"name":"discutere-status","description":"persona-engine の状態を確認"},
    {"name":"propose","description":"hypothesis 提案","options":[
      {"name":"statement","description":"仮説 1 文","type":3,"required":true}]},
    {"name":"validate","description":"hypothesis 検証","options":[
      {"name":"mode","description":"theory|emotion","type":3,"required":true}]},
    {"name":"integrate","description":"hypothesis 統合"},
    {"name":"reject","description":"hypothesis 棄却"}
  ]' \
  "https://discord.com/api/v10/applications/$DISCORD_APP_ID/commands"
```

### 4. env 設定

```sh
# kill switch を叩ける admin user id (カンマ区切り)
DISCUTERE_DISCORD_ADMIN_IDS=<your-discord-user-id>

# LLM backend を選択
# (a) Anthropic API 直叩き
LLM_BACKEND=anthropic
ANTHROPIC_API_KEY=sk-ant-...
# (b) Claude Code を Lictor 経由 spawn
LLM_BACKEND=claude-cli
# CLAUDE_CLI_TIMEOUT_MS=120000

# workspace
DISCATIER_WORKSPACE=knowledge

# session 別 safety caps
PERSONA_ENGINE_MAX_FIRES_PER_SESSION=20
PERSONA_ENGINE_MAX_FIRES_PER_RULE=5
```

### 5. server 起動

```sh
npm run build && npm start
# → :3100 で interaction endpoint 公開
```

### 6. Discord 上での運用

- `/discutere-status` — engine 状態確認 (ephemeral)
- `/discutere-kill enabled:false` — 議論停止 (admin only)
- `/discutere-kill enabled:true` — 議論再開
- `/propose statement:<仮説>` — 人間が hypothesis 提案 → persona の自動議論が走る
- `/validate mode:emotion` — 検証 → `/integrate` で採用
- 自動議論の進行は同 channel に AI persona の発話として post される

## persona-engine (議論駆動)

```sh
# 全テスト (repos / engine / Discatier adapter)
npm run test:persona-engine

# 動作 demo (MockLLM、 API 不要)
npm run persona-demo
# → gap 作成 → advocate が hypothesis 提案 → 結果 JSON

# 動作 demo (実 LLM、 ANTHROPIC_API_KEY 必須)
$env:ANTHROPIC_API_KEY = "sk-ant-..."  # PowerShell
npm run persona-demo -- --real-llm
```

エンジンは `src/persona-engine/` に閉じており、 切り出し時は `mv src/persona-engine ../persona-engine-package/src/` + `package.json` 分割で完結する設計。 Discatier 接続は `src/discatier-engine-adapter/` に隔離。

## Getting Started

### Prerequisites

- Node.js 22+
- npm
- [Cernere](https://github.com/LUDIARS/Cernere) を `../Cernere` に clone 済み (env-cli と Composite 認証サーバーとして使用)
- Infisical (環境変数管理)

### 初回セットアップ

```bash
# 1) 依存パッケージのインストール
npm install
cd frontend && npm install && cd ..

# 2) Infisical の初回設定 (初回のみ)
npm run env:setup

# 3) デフォルト値を Infisical に登録 (初回のみ)
npm run env:initialize

# 4) Cernere 側で Discutere をプロジェクト登録し、CERNERE_PROJECT_CLIENT_ID /
#    CERNERE_PROJECT_CLIENT_SECRET を Infisical に設定
npm run env:set CERNERE_PROJECT_CLIENT_ID <value>
npm run env:set CERNERE_PROJECT_CLIENT_SECRET <value>

# 5) DB スキーマ反映
npm run db:push
```

### 開発起動 (ホットリロード)

```bash
# バックエンド + フロントエンド を並列起動
npm run dev
# → api (port 3100) + web (port 5174)

# 個別起動
npm run dev:server   # バックエンドのみ
npm run dev:front    # フロントエンドのみ
```

`npm run dev` は内部で `env:env` (Infisical → `.env` 生成) → `concurrently` で api / web を起動します。

### 本番ビルド

```bash
npm run build         # バックエンド
npm start             # dist/index.js

cd frontend
npm run build         # frontend/dist/
```

## Environment Variables

`env-cli.config.ts` の `infraKeys` に定義されており、`npm run env:initialize` で
Infisical のデフォルト値として登録されます。以下が主要キーです:

| Variable | Description | Default |
|----------|------------|---------|
| `FRONTEND_PORT` | フロントエンドポート | `5174` |
| `BACKEND_PORT` | バックエンドポート | `3100` |
| `DATABASE_PATH` | SQLite DB パス | `data/discutere.db` |
| `VITE_ALLOWED_HOSTS` | Vite dev server の許可ホスト (カンマ区切り) | *(空)* |
| `FRONTEND_URL` | フロントエンド URL (CORS) | `http://localhost:5174` |
| `CERNERE_URL` | Cernere サーバー URL (Composite 認証先) | `http://localhost:8080` |
| `JWT_SECRET` | Discutere 独自 service_token の署名鍵 | `discutere-dev-secret-change-in-production` |
| `CERNERE_PROJECT_CLIENT_ID` | Cernere プロジェクト認証の client_id | — |
| `CERNERE_PROJECT_CLIENT_SECRET` | Cernere プロジェクト認証の client_secret | — |
| `ANTHROPIC_API_KEY` | `task` モードの Haiku 判定 (未設定時はルールベース) | — |
| `HAIKU_MODEL` | Haiku のモデル ID | `claude-haiku-4-5-20251001` |
| `GITHUB_TOKEN` | `discussion` モードの GitHub Discussion 書き込み用 PAT | — |

## Authentication (Cernere Composite)

認証は Cernere に委譲します。フロントエンド → Cernere ログイン → auth_code → Discutere
backend で交換 → `discutere_token` (HttpOnly Cookie) が発行されます。

フロー:
1. フロント: `POST /api/auth/login-url?origin=<self>` → Cernere ログイン URL を取得
2. Popup で Cernere ログイン → `/composite/callback?code=<authCode>` にリダイレクト
3. フロント: `POST /api/auth/exchange { code }` → auth_code を service_token に交換
4. Backend: `discutere_token` Cookie (HttpOnly, SameSite=Lax) をセット
5. 以降のリクエストは Cookie を `credentials: "include"` で送信

Backend の `/api/auth` エンドポイント:

| Method | Path | Description | 認証 |
|--------|------|-------------|------|
| `GET` | `/api/auth/login-url?origin=<url>` | Cernere ログイン URL を返す | 不要 |
| `POST` | `/api/auth/exchange` | auth_code を service_token に交換 (Cookie 設定) | 不要 |
| `POST` | `/api/auth/logout` | Cookie 削除 | 不要 |
| `GET` | `/api/auth/me` | 現在のユーザー情報 | 必須 |

## API Endpoints

### Tasks

| Method | Path | Description |
|--------|------|------------|
| `GET` | `/api/groups/:workspaceId/tasks` | タスク一覧 (status フィルタ対応) |
| `GET` | `/api/groups/:workspaceId/tasks/:taskId` | タスク詳細 + ログ |
| `POST` | `/api/groups/:workspaceId/tasks` | タスク作成 |
| `PUT` | `/api/groups/:workspaceId/tasks/:taskId` | タスク更新 |
| `DELETE` | `/api/groups/:workspaceId/tasks/:taskId` | タスク削除 |

### Channel Monitors (BOT 設定)

| Method | Path | Description |
|--------|------|------------|
| `GET` | `/api/groups/:workspaceId/monitors` | 監視チャネル一覧 (BOT 接続状態付き) |
| `POST` | `/api/groups/:workspaceId/monitors` | チャネル + BOT 認証情報を登録 |
| `PUT` | `/api/groups/:workspaceId/monitors/:id` | 監視/BOT 設定更新 |
| `DELETE` | `/api/groups/:workspaceId/monitors/:id` | チャネル監視削除 |

`POST` / `PUT` のボディに `botToken` / `botWorkspaceId` / `botSigningSecret` /
`captureMessages` を含めることで、導入済み Slack/Discord BOT の認証情報を
フロントエンドから登録できます。BOT トークンは API レスポンスでは返却されず、
`hasBotToken` フラグのみ公開されます。

チャンネルモード (`mode`: `task` / `discussion` / `none`)、議論モードの
遅延 (`discussionDelayMinutes`)、議論モードの保存先 (`githubRepo` /
`githubDiscussionCategoryId`) も同じエンドポイントで設定します。

### Channel Mode Sessions (処理状況の可視化)

| Method | Path | Description |
|--------|------|------------|
| `GET` | `/api/groups/:workspaceId/mode-sessions` | 進行中セッション一覧 (task / discussion) |
| `POST` | `/api/groups/:workspaceId/mode-sessions/task/:sessionId/resume` | ヒアリング中セッションに補足を投入して再分類 |
| `DELETE` | `/api/groups/:workspaceId/mode-sessions/task/:sessionId` | タスクモードセッションを破棄 |
| `POST` | `/api/groups/:workspaceId/mode-sessions/discussion/:sessionId/flush` | 議論モードの遅延タイマーを待たず即時実行 |
| `DELETE` | `/api/groups/:workspaceId/mode-sessions/discussion/:sessionId` | 議論モードセッションを破棄 |

セッションはすべてオンメモリ (プロセス再起動で消える) です。

### Chat Logs & Summaries

| Method | Path | Description |
|--------|------|------------|
| `GET` | `/api/groups/:workspaceId/monitors/:id/messages?limit=N` | 取得済みチャットログ一覧 |
| `GET` | `/api/groups/:workspaceId/monitors/:id/summaries` | 要約一覧 |
| `POST` | `/api/groups/:workspaceId/monitors/:id/summaries` | 期間を指定して要約を生成 |
| `DELETE` | `/api/groups/:workspaceId/monitors/:id/summaries/:summaryId` | 要約削除 |

要約生成の POST ボディ:
- `hours`: 直近 N 時間 (既定: 24)
- あるいは `periodStart` / `periodEnd` (ISO 8601)

### Webhooks & Utilities

| Method | Path | Description |
|--------|------|------------|
| `POST` | `/api/webhook/slack` | Slack Event API 受信 |
| `POST` | `/api/webhook/discord` | Discord Webhook 受信 |
| `POST` | `/api/analyze` | テキスト解析プレビュー |
| `POST` | `/api/groups/:workspaceId/tasks/:taskId/relay` | 外部 PM へリレー |
| `GET` | `/api/status` | モジュールステータス |
| `GET` | `/api/health` | ヘルスチェック |

## Message Analysis

以下のパターンでメッセージからタスクを検出します:

**タスク検出キーワード:** `task:`, `TODO:`, `お願い:`, `...をお願い`, `...してください`, `issue:`, `bug:`, `/task`, `!task`

**優先度の自動判定:**
- Critical — `急ぎ`, `至急`, `ASAP`, `緊急`
- High — `重要`, `important`
- Low — `できれば`, `低優先度`

**期限の自動抽出:** `今日中`, `明日まで`, `今週中`, `来週中`, `◯日後`, `◯月◯日`

**完了の検出:** `完了`, `done`, `修正した`, `解決`, `resolved`, `fixed`

## Project Structure

```
src/
├── index.ts                 # エントリーポイント (Hono サーバー)
├── auth/
│   ├── composite.ts         # Cernere Composite 認証フロー
│   └── routes.ts            # /api/auth/{login-url,exchange,logout,me}
├── middleware/auth.ts       # userContext / requireRole / getUserId 等
├── machina/
│   ├── routes.ts            # API ルーティング
│   ├── analyzer.ts          # テキスト解析エンジン (ルールベース)
│   ├── haiku-classifier.ts  # Haiku によるタスク性判定 (ANTHROPIC_API_KEY があれば使用)
│   ├── task-mode.ts         # チャンネルモード "task" の処理器
│   ├── discussion-mode.ts   # チャンネルモード "discussion" の処理器
│   ├── mode-state.ts        # オンメモリのセッションストア
│   ├── chat-reply.ts        # Slack / Discord への返信ヘルパ
│   ├── github-discussion.ts # 議論モードの GitHub Discussion 書き込み
│   ├── summarizer.ts        # 要約エンジン
│   ├── webhook-handler.ts   # Slack/Discord Webhook ハンドラ
│   └── pm-relay.ts          # 外部 PM 連携アダプター
├── db/
│   ├── schema.ts            # Drizzle スキーマ定義
│   ├── connection.ts        # DB 接続
│   └── repository.ts        # データアクセス層
└── shared/constants.ts      # 定数・Enum (CHANNEL_MODES を含む)

frontend/src/
├── App.tsx                       # ルーティング (PrivateRoute でガード)
├── contexts/AuthContext.tsx      # Cernere Composite ログイン + /me 同期
├── pages/
│   ├── LoginPage.tsx             # "Cernere でログイン" ボタン (popup)
│   ├── CallbackPage.tsx          # /composite/callback — authCode 受領
│   └── MachinaPage.tsx           # メイン UI
├── lib/
│   ├── constants.ts              # API_BASE
│   ├── api.ts                    # Fetch helpers (credentials: include)
│   └── api-types.ts              # 型定義
└── main.tsx                      # エントリーポイント
```

## AIFormat 準拠

LUDIARS [AIFormat](https://github.com/LUDIARS/AIFormat) の基盤設計ルールに従います:
- **認証**: Cernere Composite (HttpOnly Cookie + 独自 service_token)
- **技術スタック**: Hono + TypeScript + Drizzle ORM + React 19 + Vite
- **環境変数**: Infisical + `@cernere/env-cli`
- **npm scripts**: `concurrently` + `dotenv-cli` でクロスプラットフォーム対応
- **DB**: Drizzle ORM (現状 SQLite。PostgreSQL 移行は将来課題)

## License

[MIT](LICENSE) &copy; LUDIARS
