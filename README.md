# Discutere

Chat-to-Task / Chat-to-Discussion 自動化サービス — Slack / Discord のメッセージを解析してタスクを自動生成、 議論モードでは GitHub Discussions に要約を保存します。

**設計方針 (2026-05-28 更新)**: 元々あった Cernere + Frontend (admin UI) は撤去 (Di-14)。 今後の admin / 議論コマンドは Discord slash command + guild role guard に統一していきます。 過渡的に MACHINA admin REST は X-User-Id ヘッダーだけで動く形に縮退しています。

MACHINA (M3) モジュールとして、 外部プロジェクト管理システムとの連携にも対応。 さらに Discatier Core (src/core/) を内包し、 Game design のための 3 軸対話 (学習 / 感情 / 統合) を扱う Knowledge graph レイヤを実装中です。

## Tech Stack

| Layer | Technology |
|-------|-----------|
| Backend | [Hono](https://hono.dev/) + Node.js 22+ (TypeScript) |
| Database | SQLite (WAL) + [Drizzle ORM](https://orm.drizzle.team/) |
| Graph (Discatier) | Kuzu + ベクトル ARRAY カラム |
| Auth | Discord guild role (Di-1 以降) / 過渡期は X-User-Id ヘッダーのみ |
| Env/Secrets | [Infisical](https://infisical.com) + `@cernere/env-cli` |

## Features

- **チャットからタスク自動生成** — Slack / Discord の Webhook を受信し、 パターンマッチングでタスクを検出・作成
- **チャンネルモード (task / discussion / none)** — 各チャンネル(ツリー)に対して「何をするか」を設定
  - `task`: 投稿を即時処理。 Haiku でタスク性を判定し、 情報不足時は Slack スレッド / Discord リプライ+メンションでヒアリング。
  - `discussion`: 投稿後 N 分 (既定 5 分、 debounce) で遅延処理。 チャンネル全体を要約し GitHub Discussions に保存。
  - `none`: 何もしない (ログ保存のみ)
- **タスク管理 (CRUD)** — ステータス・優先度・担当者・期限などを管理
- **チャットログ / 要約** — 監視チャネルで流れるメッセージを蓄積、 期間指定の要約も生成
- **外部 PM 連携** — アダプターパターンで外部プロジェクト管理サービスへタスクをリレー
- **Discatier Core** — 学習 / 感情 / 統合 の 3 軸対話を扱う Knowledge graph + Event Sourcing (Phase 0-6 ライブラリ完成、 Discord 接続は Di-1 以降)

## Getting Started

### Prerequisites

- Node.js 22+
- npm
- Infisical (環境変数管理)
- [Cernere/packages/env-cli](https://github.com/LUDIARS/Cernere) を `../Cernere` に clone 済み (env-cli の参照のみ。 Cernere サーバ自体への接続は不要)

### 初回セットアップ

```bash
# 1) 依存パッケージのインストール
npm install

# 2) Infisical の初回設定 (初回のみ)
npm run env:setup

# 3) デフォルト値を Infisical に登録 (初回のみ)
npm run env:initialize

# 4) DB スキーマ反映
npm run db:push
```

### 開発起動 (ホットリロード)

```bash
npm run dev
# → api (port 3100)
```

`npm run dev` は内部で Infisical → `.env` を生成し、 `tsx watch src/index.ts` で backend を起動します。

### 本番ビルド

```bash
npm run build         # tsc
npm start             # dist/index.js
```

## Environment Variables

`env-cli.config.ts` の `infraKeys` に定義されており、 `npm run env:initialize` で
Infisical のデフォルト値として登録されます。 以下が主要キーです:

| Variable | Description | Default |
|----------|------------|---------|
| `BACKEND_PORT` | バックエンドポート | `3100` |
| `DATABASE_PATH` | SQLite DB パス | `data/discutere.db` |
| `ANTHROPIC_API_KEY` | `task` モードの Haiku 判定 (未設定時はルールベース) | — |
| `HAIKU_MODEL` | Haiku のモデル ID | `claude-haiku-4-5-20251001` |
| `GITHUB_TOKEN` | `discussion` モードの GitHub Discussion 書き込み用 PAT | — |

## Authentication (過渡期)

Cernere Composite と React Frontend は撤去済 (Di-14)。 現在は REST admin API を **X-User-Id / X-User-Role ヘッダーだけ** で認可する縮退状態です。

正式な認可は Di-1 で Discord Interactions endpoint + Ed25519 署名検証 + guild role guard に移行します。 それまで REST admin を使う場合は内部ネットワーク or localhost に限定してください。

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
`captureMessages` を含めることで BOT 認証情報を登録できます。 BOT トークンは API レスポンスでは返却されず、 `hasBotToken` フラグのみ公開されます。

チャンネルモード (`mode`: `task` / `discussion` / `none`)、 議論モードの遅延 (`discussionDelayMinutes`)、 保存先 (`githubRepo` / `githubDiscussionCategoryId`) も同じエンドポイントで設定します。

### Channel Mode Sessions (処理状況)

| Method | Path | Description |
|--------|------|------------|
| `GET` | `/api/groups/:workspaceId/mode-sessions` | 進行中セッション一覧 (task / discussion) |
| `POST` | `/api/groups/:workspaceId/mode-sessions/task/:sessionId/resume` | ヒアリング中セッションに補足を投入して再分類 |
| `DELETE` | `/api/groups/:workspaceId/mode-sessions/task/:sessionId` | タスクモードセッションを破棄 |
| `POST` | `/api/groups/:workspaceId/mode-sessions/discussion/:sessionId/flush` | 議論モードの遅延タイマーを待たず即時実行 |
| `DELETE` | `/api/groups/:workspaceId/mode-sessions/discussion/:sessionId` | 議論モードセッションを破棄 |

セッションはすべてオンメモリ (プロセス再起動で消える) です — Di-9 で SQLite 永続化予定。

### Chat Logs & Summaries

| Method | Path | Description |
|--------|------|------------|
| `GET` | `/api/groups/:workspaceId/monitors/:id/messages?limit=N` | 取得済みチャットログ一覧 |
| `GET` | `/api/groups/:workspaceId/monitors/:id/summaries` | 要約一覧 |
| `POST` | `/api/groups/:workspaceId/monitors/:id/summaries` | 期間を指定して要約を生成 |
| `DELETE` | `/api/groups/:workspaceId/monitors/:id/summaries/:summaryId` | 要約削除 |

### Webhooks & Utilities

| Method | Path | Description |
|--------|------|------------|
| `POST` | `/api/webhook/slack` | Slack Event API 受信 |
| `POST` | `/api/webhook/discord` | Discord Webhook 受信 |
| `POST` | `/api/analyze` | テキスト解析プレビュー |
| `POST` | `/api/groups/:workspaceId/tasks/:taskId/relay` | 外部 PM へリレー |
| `GET` | `/health` | ヘルスチェック |

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
├── middleware/auth.ts       # X-User-Id / X-User-Role ヘッダー読取 (Cernere/JWT 撤去後の薄い実装)
├── machina/                 # M3 MACHINA: chat → task/discussion
│   ├── routes.ts            # API ルーティング
│   ├── analyzer.ts          # テキスト解析エンジン (ルールベース)
│   ├── haiku-classifier.ts  # Haiku によるタスク性判定
│   ├── task-mode.ts         # チャンネルモード "task" の処理器
│   ├── discussion-mode.ts   # チャンネルモード "discussion" の処理器
│   ├── mode-state.ts        # オンメモリのセッションストア
│   ├── chat-reply.ts        # Slack / Discord への返信ヘルパ
│   ├── github-discussion.ts # 議論モードの GitHub Discussion 書き込み
│   ├── summarizer.ts        # 要約エンジン
│   ├── webhook-handler.ts   # Slack/Discord Webhook ハンドラ
│   └── pm-relay.ts          # 外部 PM 連携アダプター
├── discord-hook/            # Discord inbound payload 正規化 (機械的変換のみ)
├── core/                    # Discatier Core (3 軸対話 Knowledge graph、 Phase 0-6 完了)
│   ├── db/                  # Kuzu / SQLite schema
│   ├── events/              # Event Sourcing
│   ├── repositories/        # Node CRUD
│   ├── vectors/             # 埋め込み登録 / 類似検索
│   ├── bridge/              # Translation Bridge + Gap Detection
│   ├── hypothesis/          # 状態機械 + 検証ルーティング
│   ├── projection/          # メッセージ → グラフ射影 + cross queries
│   ├── jobs/                # バッチジョブ
│   ├── cache/               # ホットスポット / クラスタリングキャッシュ
│   └── index.ts             # createCore() ファクトリ
├── ludus/                   # Mechanics learner / objective opinion (stub)
├── db/                      # MACHINA 側 Drizzle スキーマ + repository
└── shared/constants.ts      # 定数・Enum
```

## License

[MIT](LICENSE) &copy; LUDIARS
