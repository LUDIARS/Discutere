# Discatier 設定ガイド

## 1. Backend / Frontend 基本設定
- `FRONTEND_PORT`: フロントのポート
- `BACKEND_PORT`: APIのポート
- `DATABASE_PATH`: SQLiteファイル
- `FRONTEND_URL`: CORS許可元
- `JWT_SECRET`: Discatier内部JWTシークレット

## 2. Discord 連携設定（Bot運用）
Discord Developer Portal で Bot を作成し、以下を準備します。

- Bot Token
- Application ID
- Guild ID（運用サーバ）

このリポジトリでは monitor 設定APIで次を投入します。
- `platform`: `discord`
- `channelId`: 監視対象チャンネルID
- `channelName`: 表示名
- `botToken`: Bot Token
- `botWorkspaceId`: Guild ID
- `captureMessages`: `true` 推奨
- `mode`: `task` / `discussion` / `none`

## 3. Discord OAuth2 URL Generator 推奨
- Scopes:
  - `bot`
  - `applications.commands`
- Bot Permissions:
  - `View Channels`
  - `Send Messages`
  - `Read Message History`
  - `Embed Links`
  - `Use Slash Commands`
  - （必要時）`Manage Channels`, `Manage Webhooks`, `Send Messages in Threads`

## 4. Ludus 辞書自動学習設定
- `LUDUS_LEARNER_ENABLED=1`
- `LUDUS_LEARNER_DEFAULT_LIMIT=20`

学習は Google News RSS 検索を利用して攻略記事候補を取得し、
タイトル/概要からメカニクス語彙を抽出して辞書保存します。

## 5. 新規 API（Ludus辞書）
- `POST /api/groups/:workspaceId/ludus/learn`
  - body: `{ "gameTitle": "GameName", "query": "ゲーム名 攻略", "limit": 20 }`
  - 非同期ジョブを起動し `jobId` を返す
- `GET /api/groups/:workspaceId/ludus/jobs`
  - 学習ジョブ一覧
- `GET /api/groups/:workspaceId/ludus/mechanics?gameTitle=GameName`
  - 学習済みメカニクス一覧

