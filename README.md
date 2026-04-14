# Discutere

Chat-to-Task 自動化サービス — Slack / Discord のメッセージを解析し、タスクを自動生成・管理します。

MACHINA (M3) モジュールとして、外部プロジェクト管理システムとの連携にも対応しています。

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Backend | [Hono](https://hono.dev/) + Node.js 22+ (TypeScript) |
| Frontend | React 19 + React Router 7 + Vite |
| Database | SQLite (WAL) + [Drizzle ORM](https://orm.drizzle.team/) |
| Auth | [Cernere](https://github.com/LUDIARS/Cernere) Composite (HttpOnly Cookie + 独自 JWT) |
| Env/Secrets | [Infisical](https://infisical.com) + `@cernere/env-cli` |

## Features

- **チャットからタスク自動生成** — Slack / Discord の Webhook を受信し、パターンマッチングでタスクを検出・作成
- **チャンネルモード (task / discussion / none)** — 各チャンネル(ツリー)に対して「何をするか」を設定
  - `task`: 投稿を即時処理。Haiku でタスク性を判定し、情報不足時は Slack スレッド / Discord リプライ+メンションでヒアリング。処理中は追加投稿を同一タスクへ取り込み、登録完了で終了。処理状態はすべてオンメモリ。
  - `discussion`: 投稿後 N 分 (既定 5 分、debounce) で遅延処理。チャンネル全体を要約し GitHub Discussions に保存。
  - `none`: 何もしない (ログ保存のみ)
- **処理状況の可視化** — タスクモードのヒアリング待ち、議論モードのタイマー待ち/失敗をフロントエンドに一覧表示し、補足投入・即時実行・破棄などの対応指示を出せる
- **タスク管理 (CRUD)** — ステータス・優先度・担当者・期限などを管理
- **BOT チャネル設定** — フロントエンドから Slack / Discord の BOT トークンを登録して監視チャネルをセットアップ
- **チャットログ** — 監視チャネルで流れるメッセージを蓄積・閲覧
- **チャット要約** — 期間を指定してメッセージを要約 (参加者統計・頻出キーワード付き)
- **テキスト解析プレビュー** — メッセージがどう解析されるかを事前確認
- **外部 PM 連携** — アダプターパターンで外部プロジェクト管理サービスへタスクをリレー
- **監査ログ** — タスクの変更履歴を自動記録

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
