# Discutere

Chat-to-Task 自動化サービス — Slack / Discord のメッセージを解析し、タスクを自動生成・管理します。

MACHINA (M3) モジュールとして、外部プロジェクト管理システムとの連携にも対応しています。

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Backend | [Hono](https://hono.dev/) + Node.js (TypeScript) |
| Frontend | React 19 + React Router + Vite |
| Database | SQLite (WAL) + [Drizzle ORM](https://orm.drizzle.team/) |
| Auth | Cernere 連携 (JWT / WebSocket 3-point auth) |

## Features

- **チャットからタスク自動生成** — Slack / Discord の Webhook を受信し、パターンマッチングでタスクを検出・作成
- **タスク管理 (CRUD)** — ステータス・優先度・担当者・期限などを管理
- **BOT チャネル設定** — フロントエンドから Slack / Discord の BOT トークンを登録して監視チャネルをセットアップ
- **チャットログ** — 監視チャネルで流れるメッセージを蓄積・閲覧
- **チャット要約** — 期間を指定してメッセージを要約 (参加者統計・頻出キーワード付き)
- **テキスト解析プレビュー** — メッセージがどう解析されるかを事前確認
- **外部 PM 連携** — アダプターパターンで外部プロジェクト管理サービスへタスクをリレー
- **監査ログ** — タスクの変更履歴を自動記録

## Getting Started

### Prerequisites

- Node.js 20+
- npm

### Setup

```bash
# 依存パッケージのインストール
npm install
cd frontend && npm install && cd ..

# 環境変数の設定
cp .env.example .env
# .env を編集して必要な値を設定

# データベースのセットアップ
npm run db:push
```

### Development

```bash
# バックエンド (port 3100)
npm run dev

# フロントエンド (port 5174) — 別ターミナルで
cd frontend
npm run dev
```

### Production Build

```bash
# バックエンド
npm run build
npm start

# フロントエンド
cd frontend
npm run build
```

## Environment Variables

| Variable | Description | Default |
|----------|------------|---------|
| `BACKEND_PORT` | サーバーポート | `3100` |
| `DATABASE_PATH` | SQLite データベースパス | `data/discutere.db` |
| `CERNERE_WS_URL` | Cernere WebSocket URL | `ws://localhost:8080/ws/service` |
| `CERNERE_SERVICE_CODE` | サービス識別コード | `discutere` |
| `CERNERE_SERVICE_SECRET` | サービス認証シークレット | — |
| `SERVICE_JWT_SECRET` | JWT 署名用シークレット | `discutere-dev-secret` |
| `FRONTEND_URL` | フロントエンド URL (CORS) | `http://localhost:5174` |

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
├── middleware/auth.ts        # 認証ミドルウェア
├── machina/
│   ├── routes.ts            # API ルーティング
│   ├── analyzer.ts          # テキスト解析エンジン
│   ├── webhook-handler.ts   # Slack/Discord Webhook ハンドラ
│   └── pm-relay.ts          # 外部 PM 連携アダプター
├── db/
│   ├── schema.ts            # Drizzle スキーマ定義
│   ├── connection.ts        # DB 接続
│   └── repository.ts        # データアクセス層
└── shared/constants.ts      # 定数・Enum

frontend/src/
├── App.tsx                  # ルーティング設定
├── pages/MachinaPage.tsx    # メイン UI
└── main.tsx                 # エントリーポイント
```

## License

[MIT](LICENSE) &copy; LUDIARS
